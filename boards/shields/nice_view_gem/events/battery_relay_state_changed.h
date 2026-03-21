/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

/*
 * Published by battery_relay_peripheral.c when the dongle writes an updated
 * battery level to the peripheral's relay GATT characteristic.
 *
 * source           – peripheral index from the dongle's perspective
 *                    (0 = left half, 1 = right half)
 * state_of_charge  – battery level 0–100 %
 */
struct zmk_battery_relay_state_changed {
    uint8_t source;
    uint8_t state_of_charge;
};

ZMK_EVENT_DECLARE(zmk_battery_relay_state_changed);
