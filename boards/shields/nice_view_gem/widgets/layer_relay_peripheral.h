/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>

/* Returns the most recently relayed layer index (0 if no relay received yet). */
uint8_t zmk_layer_relay_get_index(void);
