/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/bluetooth/uuid.h>
#include <zephyr/toolchain/common.h>

struct battery_relay_data {
    uint8_t source;
    uint8_t level;
} __packed;

/* Service UUID: 6e400010-b5a3-f393-e0a9-e50e24dcca9e */
#define BATTERY_RELAY_SERVICE_UUID_VALUE \
    BT_UUID_128_ENCODE(0x6e400010, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e)

/* Characteristic UUID: 6e400011-b5a3-f393-e0a9-e50e24dcca9e */
#define BATTERY_RELAY_CHAR_UUID_VALUE \
    BT_UUID_128_ENCODE(0x6e400011, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e)

/* Reserved source IDs used for multiplexed relay metadata. */
#define BATTERY_RELAY_SOURCE_LAYER_MARKER  0xFE
#define BATTERY_RELAY_SOURCE_DONGLE_MARKER 0xFF

/* Valid source range reserved for peripheral battery reports. */
#define BATTERY_RELAY_PERIPHERAL_SOURCE_MIN   0u
#define BATTERY_RELAY_PERIPHERAL_SOURCE_COUNT 2u
#define BATTERY_RELAY_PERIPHERAL_SOURCE_MAX   \
    (BATTERY_RELAY_PERIPHERAL_SOURCE_MIN + BATTERY_RELAY_PERIPHERAL_SOURCE_COUNT - 1u)

static inline bool battery_relay_source_is_peripheral(uint8_t source) {
    return source >= BATTERY_RELAY_PERIPHERAL_SOURCE_MIN &&
           source <= BATTERY_RELAY_PERIPHERAL_SOURCE_MAX;
}
