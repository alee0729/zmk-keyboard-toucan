/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>

/*
 * Display Relay — peripheral (left half) side.
 *
 * Exposes a writable GATT characteristic.  When the dongle writes a
 * battery_relay_data packet the peripheral caches it and raises the
 * appropriate event (battery or layer) so the display can update.
 */

/* Return the last relayed battery level for the given peripheral source index.
 * Returns 0 if no data has been received yet. */
uint8_t zmk_battery_relay_get_level(uint8_t source);

/* Return the last relayed layer index.
 * Returns 0 if no data has been received yet. */
uint8_t zmk_layer_relay_get_index(void);
