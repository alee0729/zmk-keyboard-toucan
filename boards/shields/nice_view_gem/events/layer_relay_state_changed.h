/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

/*
 * Published by layer_relay_peripheral.c when the dongle writes an updated
 * layer index to the peripheral's relay GATT characteristic.
 *
 * layer – highest active layer index from the dongle's perspective
 */
struct zmk_layer_relay_state_changed {
    uint8_t layer;
};

ZMK_EVENT_DECLARE(zmk_layer_relay_state_changed);
