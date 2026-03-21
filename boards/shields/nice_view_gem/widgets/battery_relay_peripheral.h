/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>

/*
 * Battery Relay — peripheral (left half) side.
 *
 * Exposes a writable GATT characteristic with the same UUID as the central
 * side (battery_relay_central.h in zmk-dongle-screen).  When the dongle
 * writes a battery_relay_data packet the peripheral caches it and raises a
 * zmk_battery_relay_state_changed event so the display can update.
 */

/* Return the last relayed battery level for the given peripheral source index.
 * Returns 0 if no data has been received yet. */
uint8_t zmk_battery_relay_get_level(uint8_t source);
