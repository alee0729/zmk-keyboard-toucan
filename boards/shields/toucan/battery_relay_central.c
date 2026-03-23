/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Display Relay — central (dongle) side.
 *
 * Relays battery levels AND layer state to all connected peripherals via a
 * single GATT characteristic.  Layer data is multiplexed through the battery
 * relay characteristic using source = BATTERY_RELAY_SOURCE_LAYER (0xFE).
 *
 * Discovery flow per connection:
 *   1. BT_CONN_CB_DEFINE.connected  → schedule delayed work (500 ms) so
 *      ZMK's own GATT discovery can finish first.
 *   2. Delayed work fires           → bt_gatt_discover (CHARACTERISTIC scan)
 *      for BATTERY_RELAY_CHAR_UUID.
 *   3. Discovery callback           → store value handle, push cached state.
 *   4. Battery/layer events         → write to all discovered connections.
 *
 * A periodic rebroadcast (every 60 s) pushes cached state to all peripherals
 * to recover from any dropped BLE writes.
 *
 * IMPORTANT: This module uses a single BT_CONN_CB and a single bt_conn_ref()
 * per connection to avoid exhausting BLE connection reference slots and to
 * prevent concurrent GATT discovery collisions.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>

#include <zmk/event-manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/split/central.h>

#include "battery_relay_central.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define RELAY_MAX_CONNS ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT
#define RELAY_DISCOVERY_DELAY_MS 500
#define RELAY_PERIODIC_BROADCAST_MS 60000

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

/* Cached state — sent to peripherals after discovery and periodically */
static uint8_t battery_cache[RELAY_MAX_CONNS];
static uint8_t layer_cache;

/* Async broadcast work — avoids blocking the event handler */
static struct k_work broadcast_work;
static struct k_work_delayable periodic_broadcast_work;

/* ------------------------------------------------------------------ */
/* GATT write helpers                                                   */
/* ------------------------------------------------------------------ */

static void write_to_relay(struct relay_conn_info *info, uint8_t source, uint8_t level) {
    if (info->conn == NULL || info->handle == 0) {
        return;
    }
    struct battery_relay_data data = {
        .source = source,
        .level  = level,
    };
    int err = bt_gatt_write_without_response(info->conn, info->handle,
                                             &data, sizeof(data), false);
    if (err) {
        LOG_DBG("relay write src %u err %d", source, err);
    }
}

static void push_all_cached_state(void) {
    for (int i = 0; i < RELAY_MAX_CONNS; i++) {
        if (relay_conns[i].conn == NULL || relay_conns[i].handle == 0) {
            continue;
        }
        /* Push battery levels */
        for (int src = 0; src < RELAY_MAX_CONNS; src++) {
            write_to_relay(&relay_conns[i], (uint8_t)src, battery_cache[src]);
        }
        /* Push layer */
        write_to_relay(&relay_conns[i], BATTERY_RELAY_SOURCE_LAYER, layer_cache);
    }
}

static void broadcast_work_fn(struct k_work *work) {
    push_all_cached_state();
}

/* ------------------------------------------------------------------ */
/* GATT discovery                                                       */
/* ------------------------------------------------------------------ */

static uint8_t char_discover_cb(struct bt_conn *conn,
                                 const struct bt_gatt_attr *attr,
                                 struct bt_gatt_discover_params *params) {
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
            LOG_INF("Relay char found on slot %d, handle 0x%04x",
                    i, relay_conns[i].handle);
            /* Push all cached state to newly discovered peripheral */
            for (int src = 0; src < RELAY_MAX_CONNS; src++) {
                write_to_relay(&relay_conns[i], (uint8_t)src, battery_cache[src]);
            }
            write_to_relay(&relay_conns[i], BATTERY_RELAY_SOURCE_LAYER, layer_cache);
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
/* Periodic rebroadcast                                                 */
/* ------------------------------------------------------------------ */

static void periodic_broadcast_fn(struct k_work *work) {
    push_all_cached_state();
    k_work_schedule(&periodic_broadcast_work, K_MSEC(RELAY_PERIODIC_BROADCAST_MS));
}

/* ------------------------------------------------------------------ */
/* BT connection callbacks                                              */
/* ------------------------------------------------------------------ */

static void relay_connected(struct bt_conn *conn, uint8_t err) {
    if (err) {
        return;
    }

    /* Only track connections where we are the central (keyboard halves).
     * Skip connections where we are the peripheral (BLE hosts/computers)
     * so they don't steal relay slots. */
    struct bt_conn_info conn_info;
    if (bt_conn_get_info(conn, &conn_info) != 0 ||
        conn_info.role != BT_CONN_ROLE_CENTRAL) {
        return;
    }

    for (int i = 0; i < RELAY_MAX_CONNS; i++) {
        if (relay_conns[i].conn == NULL) {
            relay_conns[i].conn        = bt_conn_ref(conn);
            relay_conns[i].handle      = 0;
            relay_conns[i].discovering = false;
            /* Delay discovery so ZMK's split GATT discovery can finish */
            k_work_schedule(&relay_conns[i].discovery_work,
                            K_MSEC(RELAY_DISCOVERY_DELAY_MS));
            break;
        }
    }
}

static void relay_disconnected(struct bt_conn *conn, uint8_t reason) {
    for (int i = 0; i < RELAY_MAX_CONNS; i++) {
        if (relay_conns[i].conn == conn) {
            k_work_cancel_delayable(&relay_conns[i].discovery_work);
            bt_conn_unref(relay_conns[i].conn);
            relay_conns[i].conn        = NULL;
            relay_conns[i].handle      = 0;
            relay_conns[i].discovering = false;
            break;
        }
    }
}

BT_CONN_CB_DEFINE(relay_conn_callbacks) = {
    .connected    = relay_connected,
    .disconnected = relay_disconnected,
};

/* ------------------------------------------------------------------ */
/* ZMK event listeners                                                  */
/* ------------------------------------------------------------------ */

static int relay_event_handler(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *bat_ev =
        as_zmk_peripheral_battery_state_changed(eh);
    if (bat_ev != NULL) {
        if (bat_ev->source < RELAY_MAX_CONNS) {
            battery_cache[bat_ev->source] = bat_ev->state_of_charge;
            /* Schedule async broadcast instead of blocking here */
            k_work_submit(&broadcast_work);
        }
        return 0;
    }

    const struct zmk_layer_state_changed *layer_ev = as_zmk_layer_state_changed(eh);
    if (layer_ev != NULL) {
        uint8_t highest = zmk_keymap_highest_layer_active();
        layer_cache = highest;
        k_work_submit(&broadcast_work);
        return 0;
    }

    return -ENOTSUP;
}

ZMK_LISTENER(display_relay_central, relay_event_handler);
ZMK_SUBSCRIPTION(display_relay_central, zmk_peripheral_battery_state_changed);
ZMK_SUBSCRIPTION(display_relay_central, zmk_layer_state_changed);

/* ------------------------------------------------------------------ */
/* Module init                                                          */
/* ------------------------------------------------------------------ */

static int display_relay_central_init(void) {
    for (int i = 0; i < RELAY_MAX_CONNS; i++) {
        relay_conns[i].conn        = NULL;
        relay_conns[i].handle      = 0;
        relay_conns[i].discovering = false;
        k_work_init_delayable(&relay_conns[i].discovery_work, discovery_work_fn);
        battery_cache[i] = 0;
    }
    layer_cache = 0;
    k_work_init(&broadcast_work, broadcast_work_fn);
    k_work_init_delayable(&periodic_broadcast_work, periodic_broadcast_fn);
    k_work_schedule(&periodic_broadcast_work, K_MSEC(RELAY_PERIODIC_BROADCAST_MS));
    return 0;
}

SYS_INIT(display_relay_central_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
