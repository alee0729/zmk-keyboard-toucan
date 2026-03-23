/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Display Relay — peripheral (left half) side.
 *
 * The dongle discovers this GATT characteristic and writes battery_relay_data
 * packets.  Battery data (source 0..N) updates battery cache and raises
 * zmk_battery_relay_state_changed.  Layer data (source = 0xFE) updates layer
 * cache and raises zmk_layer_relay_state_changed.
 *
 * UUID pair must match battery_relay_central.h in the toucan shield:
 *   Service  6e400010-b5a3-f393-e0a9-e50e24dcca9e
 *   Char     6e400011-b5a3-f393-e0a9-e50e24dcca9e
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include "../events/battery_relay_state_changed.h"
#include "../events/layer_relay_state_changed.h"

#include "battery_relay_protocol.h"

ZMK_EVENT_IMPL(zmk_battery_relay_state_changed);
ZMK_EVENT_IMPL(zmk_layer_relay_state_changed);

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static uint8_t relay_battery_cache[BATTERY_RELAY_PERIPHERAL_SOURCE_COUNT];
static uint8_t relay_layer_cache;

uint8_t zmk_battery_relay_get_level(uint8_t source) {
    if (!battery_relay_source_is_peripheral(source)) {
        return 0;
    }

    return relay_battery_cache[source - BATTERY_RELAY_PERIPHERAL_SOURCE_MIN];
}

uint8_t zmk_layer_relay_get_index(void) {
    return relay_layer_cache;
}


static ssize_t relay_write_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    if (len != sizeof(struct battery_relay_data)) {
        LOG_WRN("relay write: unexpected length %u (expected %zu)", len,
                sizeof(struct battery_relay_data));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    const struct battery_relay_data *data = buf;

    if (data->source == BATTERY_RELAY_SOURCE_LAYER_MARKER) {
        relay_layer_cache = data->level;
        LOG_DBG("relay: layer %u", data->level);
        raise_zmk_layer_relay_state_changed((struct zmk_layer_relay_state_changed){
            .layer = data->level,
        });
        return len;
    }

    if (data->source == BATTERY_RELAY_SOURCE_DONGLE_MARKER) {
        return len;
    }

    if (battery_relay_source_is_peripheral(data->source)) {
        relay_battery_cache[data->source - BATTERY_RELAY_PERIPHERAL_SOURCE_MIN] = data->level;
        LOG_DBG("relay: source %u battery %u%%", data->source, data->level);
        raise_zmk_battery_relay_state_changed((struct zmk_battery_relay_state_changed){
            .source          = data->source,
            .state_of_charge = data->level,
        });
        return len;
    }

    LOG_WRN("relay: unknown source=%u level=%u", data->source, data->level);

    return len;
}

static struct bt_uuid_128 relay_svc_uuid =
    BT_UUID_INIT_128(BATTERY_RELAY_SERVICE_UUID_VALUE);

static struct bt_uuid_128 relay_char_uuid =
    BT_UUID_INIT_128(BATTERY_RELAY_CHAR_UUID_VALUE);

BT_GATT_SERVICE_DEFINE(battery_relay_svc,
    BT_GATT_PRIMARY_SERVICE(&relay_svc_uuid.uuid),
    BT_GATT_CHARACTERISTIC(&relay_char_uuid.uuid,
                           BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE,
                           NULL, relay_write_cb, NULL),
);
