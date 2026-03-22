/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Layer Relay — peripheral (left half) side.
 *
 * The dongle discovers this GATT characteristic and writes a layer_relay_data
 * packet whenever the highest active layer changes.  Each write updates a local
 * cache and raises zmk_layer_relay_state_changed so the nice!view display can
 * update immediately.
 *
 * UUID pair must match layer_relay_central.h in the toucan shield:
 *   Service  6e400012-b5a3-f393-e0a9-e50e24dcca9e
 *   Char     6e400013-b5a3-f393-e0a9-e50e24dcca9e
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include "../events/layer_relay_state_changed.h"

ZMK_EVENT_IMPL(zmk_layer_relay_state_changed);

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static uint8_t relay_layer_cache = 0;

uint8_t zmk_layer_relay_get_index(void) { return relay_layer_cache; }

/* Payload written by the dongle — must match layer_relay_central.h */
struct layer_relay_data {
    uint8_t layer;
} __packed;

static ssize_t relay_write_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    if (len != sizeof(struct layer_relay_data)) {
        LOG_WRN("layer relay write: unexpected length %u", len);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    const struct layer_relay_data *data = buf;
    relay_layer_cache = data->layer;
    LOG_DBG("layer relay: layer %u", data->layer);

    raise_zmk_layer_relay_state_changed((struct zmk_layer_relay_state_changed){
        .layer = data->layer,
    });

    return len;
}

/* Service UUID: 6e400012-b5a3-f393-e0a9-e50e24dcca9e */
static struct bt_uuid_128 relay_svc_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x6e400012, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e));

/* Characteristic UUID: 6e400013-b5a3-f393-e0a9-e50e24dcca9e */
static struct bt_uuid_128 relay_char_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x6e400013, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e));

BT_GATT_SERVICE_DEFINE(layer_relay_svc,
    BT_GATT_PRIMARY_SERVICE(&relay_svc_uuid),
    BT_GATT_CHARACTERISTIC(&relay_char_uuid.uuid,
                           BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE,
                           NULL, relay_write_cb, NULL),
);
