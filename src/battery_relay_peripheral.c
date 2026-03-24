/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Battery Relay — Peripheral side
 * =================================
 * Registers a GATT service that the central (dongle) writes battery data to.
 * Each write delivers a struct battery_relay_data {source, level}.  The handler
 * stores the value in a local cache and re-raises a
 * zmk_peripheral_battery_state_changed event so the battery_status display
 * widget redraws without any further modification.
 *
 * The battery_status widget already subscribes to
 * zmk_peripheral_battery_state_changed unconditionally, so raising this event
 * here is enough to drive the display on a peripheral build.
 *
 * This is a patched copy of zmk-dongle-screen's battery_relay_peripheral.c
 * updated to use the ZMK v0.3 raise_zmk_*() event API instead of the
 * removed ZMK_EVENT_RAISE(new_zmk_*()) pattern.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>

#include "battery_relay_central.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * On peripheral builds zmk/split/central.h is not available, so the
 * zmk_peripheral_battery_state_changed event struct is not declared.
 * Provide a local declaration + implementation so we can raise it here
 * and have the battery_status widget (which subscribes unconditionally)
 * pick it up.
 */
struct zmk_peripheral_battery_state_changed {
    uint8_t source;
    uint8_t state_of_charge;
};

ZMK_EVENT_DECLARE(zmk_peripheral_battery_state_changed);
ZMK_EVENT_IMPL(zmk_peripheral_battery_state_changed);

/* -------------------------------------------------------------------------
 * Relay cache
 *
 * Indexed by data.source.  BATTERY_RELAY_SOURCE_DONGLE (0xFF) is ignored
 * here because SOURCE_OFFSET == 0 on peripheral builds and there is no slot
 * for the dongle battery.
 * ---------------------------------------------------------------------- */

static uint8_t relay_cache[CONFIG_DONGLE_SCREEN_BATTERY_RELAY_SOURCE_COUNT];
static uint8_t relayed_layer;

uint8_t battery_relay_get_layer(void) {
    return relayed_layer;
}

/* -------------------------------------------------------------------------
 * Deferred event processing via polling timer
 *
 * GATT write callbacks run in the BT RX thread context.  Raising ZMK
 * events synchronously there blocks the BLE host stack, which prevents
 * HID keypress notifications from being sent to the central.
 *
 * We use a self-rescheduling k_work_delayable timer (200 ms) that polls
 * a message queue for incoming relay data.  The GATT callback only does
 * a k_msgq_put (ISR-safe) — no k_work_submit from BT context needed.
 * ---------------------------------------------------------------------- */

#define RELAY_POLL_INTERVAL_MS 200

K_MSGQ_DEFINE(relay_msgq, sizeof(struct battery_relay_data), 8, 1);

static void relay_poll_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(relay_poll_work, relay_poll_handler);

static void relay_poll_handler(struct k_work *work) {
    struct battery_relay_data data;

    while (k_msgq_get(&relay_msgq, &data, K_NO_WAIT) == 0) {
        if (data.source == BATTERY_RELAY_SOURCE_LAYER) {
            relayed_layer = data.level;
            LOG_DBG("relay: layer=%u", relayed_layer);
            raise_zmk_layer_state_changed((struct zmk_layer_state_changed){
                .layer = relayed_layer,
                .state = true,
                .timestamp = k_uptime_get(),
            });
            continue;
        }

        if (data.source >= CONFIG_DONGLE_SCREEN_BATTERY_RELAY_SOURCE_COUNT) {
            continue;
        }

        relay_cache[data.source] = data.level;
        LOG_DBG("battery_relay: source=%u level=%u%%", data.source, data.level);

        raise_zmk_peripheral_battery_state_changed(
            (struct zmk_peripheral_battery_state_changed){
                .source = data.source,
                .state_of_charge = data.level,
            });
    }

    /* Reschedule — runs continuously from boot */
    k_work_schedule(&relay_poll_work, K_MSEC(RELAY_POLL_INTERVAL_MS));
}

static int relay_poll_init(void) {
    k_work_schedule(&relay_poll_work, K_MSEC(RELAY_POLL_INTERVAL_MS));
    return 0;
}
SYS_INIT(relay_poll_init, APPLICATION, 99);

/* -------------------------------------------------------------------------
 * GATT write handler — returns immediately, data picked up by poll timer
 * ---------------------------------------------------------------------- */

static ssize_t battery_relay_write_cb(struct bt_conn *conn,
                                       const struct bt_gatt_attr *attr,
                                       const void *buf, uint16_t len,
                                       uint16_t offset, uint8_t flags) {
    if (len != sizeof(struct battery_relay_data)) {
        LOG_WRN("battery_relay: unexpected write length %u (expected %zu)",
                len, sizeof(struct battery_relay_data));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    const struct battery_relay_data *data = buf;

    /* Ignore the dongle's own battery (no slot for it on peripheral display) */
    if (data->source == BATTERY_RELAY_SOURCE_DONGLE) {
        return len;
    }

    /* Queue data — poll timer will pick it up and raise events */
    if (k_msgq_put(&relay_msgq, data, K_NO_WAIT) != 0) {
        LOG_WRN("battery_relay: message queue full, dropping relay data");
    }

    return len;
}

/* -------------------------------------------------------------------------
 * GATT service definition
 * ---------------------------------------------------------------------- */

BT_GATT_SERVICE_DEFINE(battery_relay_svc,
    BT_GATT_PRIMARY_SERVICE(BATTERY_RELAY_SERVICE_UUID),
    BT_GATT_CHARACTERISTIC(BATTERY_RELAY_CHAR_UUID,
        BT_GATT_CHRC_WRITE_WITHOUT_RESP,
        BT_GATT_PERM_WRITE,
        NULL, battery_relay_write_cb, NULL),
);
