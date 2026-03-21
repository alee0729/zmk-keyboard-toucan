/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>

/*
 * Battery Relay Service — central (dongle) side.
 *
 * The dongle discovers this characteristic on each connected peripheral and
 * writes updated battery levels whenever any peripheral's battery changes.
 * Both peripheral indices (0 = left, 1 = right) are written so the left half
 * can display the right half's battery level on its nice!view display.
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

/* Payload written from dongle to each peripheral */
struct battery_relay_data {
    uint8_t source; /* peripheral index from dongle perspective: 0=left, 1=right */
    uint8_t level;  /* battery level 0-100 */
} __packed;
