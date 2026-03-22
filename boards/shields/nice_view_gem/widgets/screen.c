#include <zephyr/kernel.h>
#include <limits.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/events/layer_state_changed.h>
#endif
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/battery.h>
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include "../events/battery_relay_state_changed.h"
#include "../events/layer_relay_state_changed.h"
#endif
#include <zmk/display.h>
#include <zmk/display/widgets/battery_status.h>

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
#include <zmk/usb.h>
#include <zmk/events/usb_conn_state_changed.h>
#endif

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/* Output/profile APIs are only linked for non-split or split-central builds. */
#include <zmk/keymap.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/wpm_state_changed.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/split/central.h>
#endif

#include "battery.h"
#include "battery_peripheral.h"
#include "output.h"
#include "screen.h"
#include "sleep.h"

#include "layer.h"
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include "profile.h"
#endif

struct connection_status_state {
    bool connected;
};

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

#define BATTERY_RELAY_SOURCE_SLOTS 4
#define BATTERY_LEVEL_DIFF_MATCH_MAX 10
#define BATTERY_RELAY_SOURCE_UNKNOWN UINT8_MAX

struct battery_relay_source_slot {
    bool used;
    uint8_t source;
    uint8_t level;
    uint16_t local_match_score;
    int64_t last_seen_ms;
};

struct battery_relay_selection_state {
    bool local_level_valid;
    uint8_t local_level;
    uint8_t inferred_local_source;
    struct battery_relay_source_slot slots[BATTERY_RELAY_SOURCE_SLOTS];
};

static struct battery_relay_selection_state battery_relay_selection = {
    .inferred_local_source = BATTERY_RELAY_SOURCE_UNKNOWN,
};

static struct battery_relay_source_slot *get_or_create_source_slot(uint8_t source) {
    struct battery_relay_source_slot *free_slot = NULL;
    int64_t oldest_seen = INT64_MAX;
    struct battery_relay_source_slot *oldest_slot = NULL;

    for (size_t i = 0; i < ARRAY_SIZE(battery_relay_selection.slots); i++) {
        struct battery_relay_source_slot *slot = &battery_relay_selection.slots[i];
        if (slot->used && slot->source == source) {
            return slot;
        }

        if (!slot->used && free_slot == NULL) {
            free_slot = slot;
        }

        if (slot->used && slot->last_seen_ms < oldest_seen) {
            oldest_seen = slot->last_seen_ms;
            oldest_slot = slot;
        }
    }

    struct battery_relay_source_slot *slot = (free_slot != NULL) ? free_slot : oldest_slot;
    if (slot == NULL) {
        return NULL;
    }

    *slot = (struct battery_relay_source_slot){
        .used = true,
        .source = source,
    };
    return slot;
}

static void infer_local_source_from_local_level(uint8_t local_level) {
    uint16_t best_score = 0;
    uint8_t best_source = BATTERY_RELAY_SOURCE_UNKNOWN;
    uint16_t second_best_score = 0;

    for (size_t i = 0; i < ARRAY_SIZE(battery_relay_selection.slots); i++) {
        struct battery_relay_source_slot *slot = &battery_relay_selection.slots[i];
        if (!slot->used) {
            continue;
        }

        uint8_t diff = (slot->level > local_level) ? (slot->level - local_level)
                                                    : (local_level - slot->level);
        if (diff <= BATTERY_LEVEL_DIFF_MATCH_MAX) {
            slot->local_match_score += (BATTERY_LEVEL_DIFF_MATCH_MAX + 1u - diff);
        }

        if (slot->local_match_score >= best_score) {
            second_best_score = best_score;
            best_score = slot->local_match_score;
            best_source = slot->source;
        } else if (slot->local_match_score > second_best_score) {
            second_best_score = slot->local_match_score;
        }
    }

    if (best_source != BATTERY_RELAY_SOURCE_UNKNOWN && best_score > second_best_score) {
        battery_relay_selection.inferred_local_source = best_source;
    }
}

static bool is_better_remote_candidate(const struct battery_relay_source_slot *candidate,
                                       const struct battery_relay_source_slot *current) {
    if (current == NULL) {
        return true;
    }

    if (candidate->level != current->level) {
        return candidate->level > current->level;
    }

    return candidate->last_seen_ms > current->last_seen_ms;
}

static uint8_t select_remote_source_candidate(void) {
    struct battery_relay_source_slot *best_non_local = NULL;
    struct battery_relay_source_slot *best_any = NULL;

    for (size_t i = 0; i < ARRAY_SIZE(battery_relay_selection.slots); i++) {
        struct battery_relay_source_slot *slot = &battery_relay_selection.slots[i];
        if (!slot->used || slot->level == 0) {
            continue;
        }

        if (is_better_remote_candidate(slot, best_any)) {
            best_any = slot;
        }

        if (battery_relay_selection.inferred_local_source != BATTERY_RELAY_SOURCE_UNKNOWN &&
            slot->source == battery_relay_selection.inferred_local_source) {
            continue;
        }

        if (is_better_remote_candidate(slot, best_non_local)) {
            best_non_local = slot;
        }
    }

    if (best_non_local != NULL) {
        return best_non_local->source;
    }
    if (best_any != NULL) {
        return best_any->source;
    }

    return BATTERY_RELAY_SOURCE_UNKNOWN;
}

static bool get_remote_battery_level_from_sources(uint8_t *level_out) {
    uint8_t remote_source = select_remote_source_candidate();
    if (remote_source == BATTERY_RELAY_SOURCE_UNKNOWN) {
        return false;
    }

    for (size_t i = 0; i < ARRAY_SIZE(battery_relay_selection.slots); i++) {
        struct battery_relay_source_slot *slot = &battery_relay_selection.slots[i];
        if (slot->used && slot->source == remote_source) {
            *level_out = slot->level;
            return true;
        }
    }

    return false;
}

/**
 * Draw buffers
 **/

static void draw_top(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 0);
    fill_background(canvas);

    if (is_sleep_screen_active()) {
        draw_sleep_screen(canvas);
        return;
    }

    // Draw widgets
    draw_output_status(canvas, state);
    draw_layer_status(canvas, state);
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    draw_profile_status(canvas, state);
#endif
    draw_battery_status(canvas, state);
    draw_battery_peripheral_status(canvas, state);
}

/**
 * Battery status (own / left half)
 **/

static void set_battery_status(struct zmk_widget_screen *widget,
                               struct battery_status_state state) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    widget->state.charging = state.usb_present;
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
    widget->state.battery = state.level;
    battery_relay_selection.local_level_valid = true;
    battery_relay_selection.local_level = state.level;
    infer_local_source_from_local_level(state.level);

    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void battery_status_update_cb(struct battery_status_state state) {
    struct zmk_widget_screen *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_status(widget, state); }
}

static struct battery_status_state battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);

    return (struct battery_status_state){
        .level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state);

ZMK_SUBSCRIPTION(widget_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_usb_conn_state_changed);
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

/**
 * Battery peripheral status (other half)
 *
 * Handles split battery updates from any source and dynamically picks the
 * "remote" source to display by comparing source values against local battery
 * updates over time. If source identity is uncertain, it defensively prefers
 * the newest non-zero relay value.
 **/

static void set_battery_peripheral_status(struct zmk_widget_screen *widget,
                               struct battery_peripheral_status_state state) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    widget->state.charging_p = state.usb_present;
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
    struct battery_relay_source_slot *slot = get_or_create_source_slot(state.source);
    if (slot == NULL) {
        return;
    }

    slot->level = state.level;
    slot->last_seen_ms = k_uptime_get();

    if (battery_relay_selection.local_level_valid) {
        infer_local_source_from_local_level(battery_relay_selection.local_level);
    }

    uint8_t remote_level = 0;
    if (!get_remote_battery_level_from_sources(&remote_level)) {
        return;
    }

    widget->state.battery_p = remote_level;

    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void battery_peripheral_status_update_cb(struct battery_peripheral_status_state state) {
    struct zmk_widget_screen *widget;

    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_peripheral_status(widget, state); }
}

static struct battery_peripheral_status_state battery_peripheral_status_get_state(const zmk_event_t *eh) {
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    const struct zmk_peripheral_battery_state_changed *ev = as_zmk_peripheral_battery_state_changed(eh);
    return (struct battery_peripheral_status_state){
        .source = ev->source,
        .level = ev->state_of_charge,
    };
#else
    const struct zmk_battery_relay_state_changed *ev = as_zmk_battery_relay_state_changed(eh);
    return (struct battery_peripheral_status_state){
        .source = ev->source,
        .level = ev->state_of_charge,
    };
#endif
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_peripheral_status, struct battery_peripheral_status_state,
                            battery_peripheral_status_update_cb, battery_peripheral_status_get_state);

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
ZMK_SUBSCRIPTION(widget_battery_peripheral_status, zmk_peripheral_battery_state_changed);
#else
ZMK_SUBSCRIPTION(widget_battery_peripheral_status, zmk_battery_relay_state_changed);
#endif

/* Layer status */

static void set_layer_status(struct zmk_widget_screen *widget, struct layer_status_state state) {
    widget->state.layer_index = state.index;
    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void layer_status_update_cb(struct layer_status_state state) {
    struct zmk_widget_screen *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_layer_status(widget, state); }
}

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

static struct layer_status_state layer_status_get_state(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);

    return (struct layer_status_state){
        .index = (ev != NULL) ? ev->layer : 0,
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer_status, struct layer_status_state, layer_status_update_cb,
                            layer_status_get_state)

ZMK_SUBSCRIPTION(widget_layer_status, zmk_layer_state_changed);

#else /* Peripheral role: use layer relay from dongle */

static struct layer_status_state layer_relay_status_get_state(const zmk_event_t *eh) {
    const struct zmk_layer_relay_state_changed *ev = as_zmk_layer_relay_state_changed(eh);

    return (struct layer_status_state){
        .index = (ev != NULL) ? ev->layer : 0,
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer_status, struct layer_status_state, layer_status_update_cb,
                            layer_relay_status_get_state)

ZMK_SUBSCRIPTION(widget_layer_status, zmk_layer_relay_state_changed);

#endif /* !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) */

/**
 * Role-specific widgets: output status (central/non-split), connection status (peripheral split)
 **/

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/* Output status */

static void set_output_status(struct zmk_widget_screen *widget,
                              const struct output_status_state *state) {
    widget->state.selected_endpoint = state->selected_endpoint;
    widget->state.active_profile_index = state->active_profile_index;
    widget->state.active_profile_connected = state->active_profile_connected;
    widget->state.active_profile_bonded = state->active_profile_bonded;

    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void output_status_update_cb(struct output_status_state state) {
    struct zmk_widget_screen *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_output_status(widget, &state); }
}

static struct output_status_state output_status_get_state(const zmk_event_t *_eh) {
    return (struct output_status_state){
        .selected_endpoint = zmk_endpoints_selected(),
        .active_profile_index = zmk_ble_active_profile_index(),
        .active_profile_connected = zmk_ble_active_profile_is_connected(),
        .active_profile_bonded = !zmk_ble_active_profile_is_open(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_output_status, struct output_status_state,
                            output_status_update_cb, output_status_get_state)
ZMK_SUBSCRIPTION(widget_output_status, zmk_endpoint_changed);

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_output_status, zmk_usb_conn_state_changed);
#endif
#if defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(widget_output_status, zmk_ble_active_profile_changed);
#endif

#else /* Peripheral role */

/**
 * Peripheral-only: connection status
 * Tracks the split BLE connection to the dongle (central).
 * Used to display "DONGLE MODE" when connected or "DISCONNECTED" when not.
 **/

static void set_connection_status(struct zmk_widget_screen *widget,
                                  struct connection_status_state state) {
    widget->state.connected = state.connected;
    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void connection_status_update_cb(struct connection_status_state state) {
    struct zmk_widget_screen *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_connection_status(widget, state); }
}

static struct connection_status_state connection_status_get_state(const zmk_event_t *eh) {
    const struct zmk_split_peripheral_status_changed *ev =
        as_zmk_split_peripheral_status_changed(eh);
    return (struct connection_status_state){
        .connected = (ev != NULL) ? ev->connected : false,
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_connection_status, struct connection_status_state,
                            connection_status_update_cb, connection_status_get_state)

ZMK_SUBSCRIPTION(widget_connection_status, zmk_split_peripheral_status_changed);

#endif /* !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) */

/**
 * Activity state handling for sleep screen
 **/

static void force_redraw_all_widgets(void) {
    struct zmk_widget_screen *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        draw_top(widget->obj, widget->cbuf, &widget->state);
    }
}

static int display_activity_event_handler(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev == NULL) {
        return -ENOTSUP;
    }

    switch (ev->state) {
    case ZMK_ACTIVITY_ACTIVE:
        set_sleep_screen_active(false);
        break;
    case ZMK_ACTIVITY_SLEEP:
        set_sleep_screen_active(true);
        force_redraw_all_widgets();
        /* Force LVGL to process pending updates and flush to display hardware
         * before the CPU enters deep sleep */
        lv_task_handler();
        lv_refr_now(NULL);
        break;
    default:
        break; /* ignore other states (like IDLE) */
    }
    return 0;
}

ZMK_LISTENER(nice_view_gem_display, display_activity_event_handler);
ZMK_SUBSCRIPTION(nice_view_gem_display, zmk_activity_state_changed);

/**
 * Initialization
 **/

int zmk_widget_screen_init(struct zmk_widget_screen *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, SCREEN_WIDTH, SCREEN_HEIGHT);

    lv_obj_t *top = lv_canvas_create(widget->obj);
    lv_obj_align(top, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_canvas_set_buffer(top, widget->cbuf, SCREEN_WIDTH, SCREEN_HEIGHT, LV_IMG_CF_TRUE_COLOR);

    sys_slist_append(&widgets, &widget->node);
    widget_battery_status_init();
    widget_battery_peripheral_status_init();
    widget_layer_status_init();

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    widget_output_status_init();
#else
    widget_connection_status_init();
#endif

    return 0;
}

lv_obj_t *zmk_widget_screen_obj(struct zmk_widget_screen *widget) { return widget->obj; }
