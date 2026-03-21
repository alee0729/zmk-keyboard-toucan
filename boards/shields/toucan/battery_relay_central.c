/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Battery Relay — central (dongle) side.
 *
 * When any peripheral's battery level changes the dongle writes the updated
 * level to ALL connected peripherals that expose the battery relay GATT
 * characteristic.  This lets the left half's nice!view display show the right
 * half's battery level even though the left is itself a peripheral.
 *
 * Discovery flow per connection:
 *   1. BT_CONN_CB_DEFINE.connected  → schedule delayed work (500 ms) so
 *      ZMK's own GATT discovery can finish first.
 *   2. Delayed work fires           → bt_gatt_discover (CHARACTERISTIC scan,
 *      full ATT range) for BATTERY_RELAY_CHAR_UUID.
 *   3. Discovery callback           → store value handle.
 *   4. zmk_peripheral_battery_state_changed → update cache + write to all
 *      connections with a known handle.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/split/central.h>

#include "battery_relay_central.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define RELAY_MAX_CONNS ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT

/* Per-connection relay state */
struct relay_conn_info {
    struct bt_conn *conn;
    uint16_t handle;           /* 0 = not yet discovered */
    bool discovering;
    struct bt_gatt_discover_params discover_params;
    struct bt_uuid_128 discover_uuid; /* must outlive the discovery call */
    struct k_work_delayable discovery_work;
};

static struct relay_conn_info relay_conns[RELAY_MAX_CONNS];

/* Cache of the most recent battery level for each peripheral index */
static uint8_t battery_cache[RELAY_MAX_CONNS];

/* ------------------------------------------------------------------ */
/* GATT write helpers                                                   */
/* ------------------------------------------------------------------ */

static void relay_battery_to_all(void) {
    for (int i = 0; i < RELAY_MAX_CONNS; i++) {
        if (relay_conns[i].conn == NULL || relay_conns[i].handle == 0) {
            continue;
        }
        /* Write each cached level to this peripheral */
        for (int src = 0; src < RELAY_MAX_CONNS; src++) {
            struct battery_relay_data data = {
                .source = (uint8_t)src,
                .level  = battery_cache[src],
            };
            int err = bt_gatt_write_without_response(relay_conns[i].conn,
                                                     relay_conns[i].handle,
                                                     &data, sizeof(data), false);
            if (err) {
                LOG_DBG("relay write to slot %d src %d err %d", i, src, err);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* GATT discovery                                                       */
/* ------------------------------------------------------------------ */

static uint8_t char_discover_cb(struct bt_conn *conn,
                                 const struct bt_gatt_attr *attr,
                                 struct bt_gatt_discover_params *params) {
    /* Find which slot this connection belongs to */
    for (int i = 0; i < RELAY_MAX_CONNS; i++) {
        if (relay_conns[i].conn != conn) {
            continue;
        }
        relay_conns[i].discovering = false;
        if (attr == NULL) {
            LOG_DBG("relay char not found on slot %d", i);
        } else {
            struct bt_gatt_chrc *chrc = attr->user_data;
            relay_conns[i].handle = chrc->value_handle;
            LOG_INF("Battery relay char found on slot %d, handle 0x%04x",
                    i, relay_conns[i].handle);
            /* Immediately push the current cache to this peripheral */
            for (int src = 0; src < RELAY_MAX_CONNS; src++) {
                struct battery_relay_data data = {
                    .source = (uint8_t)src,
                    .level  = battery_cache[src],
                };
                bt_gatt_write_without_response(conn, relay_conns[i].handle,
                                               &data, sizeof(data), false);
            }
        }
        break;
    }
    return BT_GATT_ITER_STOP;
}

static void start_discovery(struct relay_conn_info *info) {
    static const struct bt_uuid_128 relay_char_uuid =
        BT_UUID_INIT_128(BATTERY_RELAY_CHAR_UUID);

    info->discover_uuid = relay_char_uuid;
    info->discover_params = (struct bt_gatt_discover_params){
        .uuid        = &info->discover_uuid.uuid,
        .func        = char_discover_cb,
        .start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE,
        .end_handle   = BT_ATT_LAST_ATTRIBUTE_HANDLE,
        .type         = BT_GATT_DISCOVER_CHARACTERISTIC,
    };
    info->discovering = true;

    int err = bt_gatt_discover(info->conn, &info->discover_params);
    if (err) {
        info->discovering = false;
        LOG_DBG("bt_gatt_discover err %d", err);
    }
}

static void discovery_work_fn(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct relay_conn_info *info =
        CONTAINER_OF(dwork, struct relay_conn_info, discovery_work);

    if (info->conn == NULL || info->handle != 0 || info->discovering) {
        return;
    }
    start_discovery(info);
}

/* ------------------------------------------------------------------ */
/* BT connection callbacks                                              */
/* ------------------------------------------------------------------ */

static void relay_connected(struct bt_conn *conn, uint8_t err) {
    if (err) {
        return;
    }
    /* Find a free slot */
    for (int i = 0; i < RELAY_MAX_CONNS; i++) {
        if (relay_conns[i].conn == NULL) {
            relay_conns[i].conn        = bt_conn_ref(conn);
            relay_conns[i].handle      = 0;
            relay_conns[i].discovering = false;
            /* Delay discovery so ZMK's split GATT discovery can finish */
            k_work_schedule(&relay_conns[i].discovery_work, K_MSEC(500));
            break;
        }
    }
}

static void relay_disconnected(struct bt_conn *conn, uint8_t reason) {
    for (int i = 0; i < RELAY_MAX_CONNS; i++) {
        if (relay_conns[i].conn == conn) {
            bt_conn_unref(relay_conns[i].conn);
            relay_conns[i].conn        = NULL;
            relay_conns[i].handle      = 0;
            relay_conns[i].discovering = false;
            k_work_cancel_delayable(&relay_conns[i].discovery_work);
            break;
        }
    }
}

BT_CONN_CB_DEFINE(relay_conn_callbacks) = {
    .connected    = relay_connected,
    .disconnected = relay_disconnected,
};

/* ------------------------------------------------------------------ */
/* ZMK battery event listener                                           */
/* ------------------------------------------------------------------ */

static int battery_relay_event_handler(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);
    if (ev == NULL) {
        return -ENOTSUP;
    }
    if (ev->source < RELAY_MAX_CONNS) {
        battery_cache[ev->source] = ev->state_of_charge;
        relay_battery_to_all();
    }
    return 0;
}

ZMK_LISTENER(battery_relay_central, battery_relay_event_handler);
ZMK_SUBSCRIPTION(battery_relay_central, zmk_peripheral_battery_state_changed);

/* ------------------------------------------------------------------ */
/* Module init                                                          */
/* ------------------------------------------------------------------ */

static int battery_relay_central_init(void) {
    for (int i = 0; i < RELAY_MAX_CONNS; i++) {
        relay_conns[i].conn        = NULL;
        relay_conns[i].handle      = 0;
        relay_conns[i].discovering = false;
        k_work_init_delayable(&relay_conns[i].discovery_work, discovery_work_fn);
        battery_cache[i] = 0;
    }
    return 0;
}

SYS_INIT(battery_relay_central_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
