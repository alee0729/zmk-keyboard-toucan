/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Display Relay — peripheral (left half) side.
 *
 * The dongle discovers this GATT characteristic and writes battery_relay_data
 * packets.  Battery data (source 0..N) updates battery cache and raises
 * zmk_battery_relay_state_changed.  Layer data (source = 0xFE) updates layer
 * cache and raises zmk_layer_relay_state_changed.
 *
 * UUID pair must match battery_relay_central.h in the toucan shield:
 *   Service  6e400010-b5a3-f393-e0a9-e50e24dcca9e
 *   Char     6e400011-b5a3-f393-e0a9-e50e24dcca9e
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include "../events/battery_relay_state_changed.h"
#include "../events/layer_relay_state_changed.h"

ZMK_EVENT_IMPL(zmk_battery_relay_state_changed);
ZMK_EVENT_IMPL(zmk_layer_relay_state_changed);

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Maximum number of peripheral sources we track (matches dongle's peripheral count) */
#define RELAY_MAX_SOURCES 2

/* source value indicating layer data instead of battery data */
#define BATTERY_RELAY_SOURCE_LAYER 0xFE

/* Payload written by the dongle — must match battery_relay_central.h */
struct battery_relay_data {
    uint8_t source;
    uint8_t level;
} __packed;

static uint8_t relay_battery_cache[RELAY_MAX_SOURCES];
static uint8_t relay_layer_cache;
volatile uint32_t relay_diag_write_count;
/* 0 = not attempted, 1 = registered OK, negative = error code */
volatile int relay_diag_svc_status;

/*
 * Debounced event processing.
 *
 * The GATT write callback (BT RX thread) must NOT call into ZMK's event
 * manager — doing so can deadlock.  Instead, the callback updates caches
 * and sets dirty flags.  A delayed work item fires after a short debounce
 * window and raises ONE event per dirty flag from the system work queue.
 *
 * This also coalesces rapid bursts (push_cached_state sends 3-4 writes
 * back-to-back) into a single set of event raises.
 */
#define RELAY_DEBOUNCE_MS 100

static volatile bool battery_dirty[RELAY_MAX_SOURCES];
static volatile bool layer_dirty;

static void relay_debounce_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(relay_debounce_work, relay_debounce_work_handler);

static void relay_debounce_work_handler(struct k_work *work) {
    /* Raise one event per dirty source.  Events are processed synchronously
     * by ZMK, so each is fully handled (and freed) before the next. */
    for (int i = 0; i < RELAY_MAX_SOURCES; i++) {
        if (battery_dirty[i]) {
            battery_dirty[i] = false;
            raise_zmk_battery_relay_state_changed((struct zmk_battery_relay_state_changed){
                .source          = (uint8_t)i,
                .state_of_charge = relay_battery_cache[i],
            });
        }
    }

    if (layer_dirty) {
        layer_dirty = false;
        raise_zmk_layer_relay_state_changed((struct zmk_layer_relay_state_changed){
            .layer = relay_layer_cache,
        });
    }
}

uint8_t zmk_battery_relay_get_level(uint8_t source) {
    if (source >= RELAY_MAX_SOURCES) {
        return 0;
    }
    return relay_battery_cache[source];
}

uint8_t zmk_layer_relay_get_index(void) {
    return relay_layer_cache;
}

static ssize_t relay_write_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    if (len != sizeof(struct battery_relay_data)) {
        LOG_WRN("relay write: unexpected length %u", len);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    const struct battery_relay_data *data = buf;
    relay_diag_write_count++;

    /* Update caches and set dirty flags (safe — just byte writes) */
    if (data->source == BATTERY_RELAY_SOURCE_LAYER) {
        relay_layer_cache = data->level;
        layer_dirty = true;
        LOG_DBG("relay: layer %u", data->level);
    } else if (data->source < RELAY_MAX_SOURCES) {
        relay_battery_cache[data->source] = data->level;
        battery_dirty[data->source] = true;
        LOG_DBG("relay: source %u battery %u%%", data->source, data->level);
    }

    /* Schedule (or reschedule) debounced event processing.
     * k_work_reschedule resets the timer on each write, so a burst of
     * 4 rapid writes results in only ONE work execution ~100ms after
     * the last write. */
    k_work_reschedule(&relay_debounce_work, K_MSEC(RELAY_DEBOUNCE_MS));

    return len;
}

/* Service UUID: 6e400010-b5a3-f393-e0a9-e50e24dcca9e */
static struct bt_uuid_128 relay_svc_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x6e400010, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e));

/* Characteristic UUID: 6e400011-b5a3-f393-e0a9-e50e24dcca9e */
static struct bt_uuid_128 relay_char_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x6e400011, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e));

/* Dynamic GATT service registration — static BT_GATT_SERVICE_DEFINE was
 * not making it into the GATT table (likely a linker/section issue).
 * Use bt_gatt_service_register() via delayed work to ensure BT is ready. */
static struct bt_gatt_attr relay_attrs[] = {
    BT_GATT_PRIMARY_SERVICE(&relay_svc_uuid),
    BT_GATT_CHARACTERISTIC(&relay_char_uuid.uuid,
                           BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE,
                           NULL, relay_write_cb, NULL),
};

static struct bt_gatt_service relay_svc = BT_GATT_SERVICE(relay_attrs);

static void relay_register_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(relay_register_work, relay_register_work_handler);

static void relay_register_work_handler(struct k_work *work) {
    if (!bt_is_ready()) {
        /* BT not ready yet — retry in 200 ms */
        k_work_reschedule(&relay_register_work, K_MSEC(200));
        return;
    }

    int err = bt_gatt_service_register(&relay_svc);
    relay_diag_svc_status = err ? err : 1;
    if (err) {
        LOG_ERR("relay: bt_gatt_service_register failed: %d", err);
    } else {
        LOG_INF("relay: GATT service registered OK (%u attrs)",
                (unsigned)relay_svc.attr_count);
    }
}

static int relay_peripheral_init(void) {
    /* Defer registration until BT subsystem is ready */
    k_work_schedule(&relay_register_work, K_MSEC(500));
    return 0;
}

SYS_INIT(relay_peripheral_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
