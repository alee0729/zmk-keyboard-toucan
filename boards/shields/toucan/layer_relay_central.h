/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>

/*
 * Layer Relay Service — central (dongle) side.
 *
 * The dongle discovers this characteristic on each connected peripheral and
 * writes the highest active layer index whenever a layer state change fires.
 *
 * UUID pair is shared with the peripheral side
 * (boards/shields/nice_view_gem/widgets/layer_relay_peripheral.c).
 */

/* 128-bit service UUID: 6e400012-b5a3-f393-e0a9-e50e24dcca9e */
#define LAYER_RELAY_SVC_UUID                                                                       \
    BT_UUID_128_ENCODE(0x6e400012, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e)

/* 128-bit characteristic UUID: 6e400013-b5a3-f393-e0a9-e50e24dcca9e */
#define LAYER_RELAY_CHAR_UUID                                                                      \
    BT_UUID_128_ENCODE(0x6e400013, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e)

/* Payload written from dongle to each peripheral */
struct layer_relay_data {
    uint8_t layer; /* highest active layer index */
} __packed;
