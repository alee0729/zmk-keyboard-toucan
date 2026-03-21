/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Battery Relay — peripheral (left half) side.
 *
 * The dongle discovers this GATT characteristic and writes battery_relay_data
 * packets to it whenever any peripheral's battery level changes.  Each write
 * updates a local cache and raises zmk_battery_relay_state_changed so the
 * nice!view display can update immediately.
 *
 * UUID pair must match battery_relay_central.h in zmk-dongle-screen:
 *   Service  6e400010-b5a3-f393-e0a9-e50e24dcca9e
 *   Char     6e400011-b5a3-f393-e0a9-e50e24dcca9e
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include "../events/battery_relay_state_changed.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Maximum number of peripheral sources we track (matches dongle's peripheral count) */
#define RELAY_MAX_SOURCES 2

static uint8_t relay_battery_cache[RELAY_MAX_SOURCES];

uint8_t zmk_battery_relay_get_level(uint8_t source) {
    if (source >= RELAY_MAX_SOURCES) {
        return 0;
    }
    return relay_battery_cache[source];
}

/* Payload written by the dongle — must match battery_relay_central.h */
struct battery_relay_data {
    uint8_t source;
    uint8_t level;
} __packed;

static ssize_t relay_write_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    if (len != sizeof(struct battery_relay_data)) {
        LOG_WRN("battery relay write: unexpected length %u", len);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    const struct battery_relay_data *data = buf;

    if (data->source < RELAY_MAX_SOURCES) {
        relay_battery_cache[data->source] = data->level;
        LOG_DBG("battery relay: source %u level %u%%", data->source, data->level);

        ZMK_EVENT_RAISE(new_zmk_battery_relay_state_changed(
            ((struct zmk_battery_relay_state_changed){
                .source          = data->source,
                .state_of_charge = data->level,
            })));
    }

    return len;
}

/* Service UUID: 6e400010-b5a3-f393-e0a9-e50e24dcca9e */
static struct bt_uuid_128 relay_svc_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x6e400010, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e));

/* Characteristic UUID: 6e400011-b5a3-f393-e0a9-e50e24dcca9e */
static struct bt_uuid_128 relay_char_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x6e400011, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e));

BT_GATT_SERVICE_DEFINE(battery_relay_svc,
    BT_GATT_PRIMARY_SERVICE(&relay_svc_uuid),
    BT_GATT_CHARACTERISTIC(&relay_char_uuid.uuid,
                           BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE,
                           NULL, relay_write_cb, NULL),
);
