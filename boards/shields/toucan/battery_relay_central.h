/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>

/*
 * Display Relay Service — central (dongle) side.
 *
 * The dongle discovers this characteristic on each connected peripheral and
 * writes battery levels and layer state through it.  Layer data is multiplexed
 * using source = BATTERY_RELAY_SOURCE_LAYER (0xFE).
 *
 * UUID pair is shared with the peripheral side
 * (boards/shields/nice_view_gem/widgets/battery_relay_peripheral.c).
 */

/* 128-bit service UUID: 6e400010-b5a3-f393-e0a9-e50e24dcca9e */
#define BATTERY_RELAY_SVC_UUID                                                                     \
    BT_UUID_128_ENCODE(0x6e400010, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e)

/* 128-bit characteristic UUID: 6e400011-b5a3-f393-e0a9-e50e24dcca9e */
#define BATTERY_RELAY_CHAR_UUID                                                                    \
    BT_UUID_128_ENCODE(0x6e400011, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e)

/* source value used to carry layer data through the battery relay characteristic.
 * When source == BATTERY_RELAY_SOURCE_LAYER, the level field contains the
 * highest active layer index instead of a battery percentage. */
#define BATTERY_RELAY_SOURCE_LAYER 0xFE

/* Payload written from dongle to each peripheral */
struct battery_relay_data {
    uint8_t source; /* peripheral index (0=left, 1=right), or BATTERY_RELAY_SOURCE_LAYER */
    uint8_t level;  /* battery level 0-100, or layer index when source=0xFE */
} __packed;
