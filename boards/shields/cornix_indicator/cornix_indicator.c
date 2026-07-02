/*
 * Cornix two-LED status indicator.
 * Ported from the RMK implementation in rmk-cornix/src/ws2812.rs.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <hal/nrf_power.h>

#include <zmk/activity.h>
#include <zmk/ble.h>
#include <zmk/battery.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/workqueue.h>

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <dt-bindings/zmk/hid_indicators.h>
#include <zmk/events/hid_indicators_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/transport/central.h>
#else
#include <zmk/split/bluetooth/peripheral.h>
#endif

LOG_MODULE_REGISTER(cornix_indicator, CONFIG_ZMK_LOG_LEVEL);

#define LED_STRIP_NODE DT_ALIAS(status_ws2812)
#define LED_POWER_NODE DT_ALIAS(status_led_power)

#define ANIM_INTERVAL_MS 33
#define IDLE_INTERVAL_MS 1000
#define NOTICE_MS 3000
#define ACTIVITY_MS 60000
#define LOW_ALERT_MS 5000
#define LOW_QUIET_MS (5 * 60 * 1000)
#define BREATH_STEPS 64
#define BREATH_PERIOD_MS 3000
#define LOW_BLINK_PERIOD_MS 1200
#define FADE_STEP 3
#define BATTERY_LOW 20
#define BATTERY_FULL 95

struct color {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

enum effect_kind {
    EFFECT_OFF,
    EFFECT_SOLID,
    EFFECT_BREATH,
    EFFECT_LOW_BATTERY,
};

struct effect {
    enum effect_kind kind;
    struct color color;
};

struct indicator_state {
    bool initialized;
    bool pending;
    bool battery_known;
    uint8_t battery;
    bool charging;
    uint8_t ble_profile;
    bool ble_connected;
    bool ble_advertising;
    bool peer_connected;
    bool caps_lock;
    bool sleeping;

    int64_t ble_since;
    int64_t peer_since;
    int64_t charge_since;
    int64_t low_alert_until;
    int64_t low_next_alert;

    struct color cur_inner;
    struct color cur_outer;
    struct color tgt_inner;
    struct color tgt_outer;
    bool inner_active;
    bool outer_active;

    bool rail_on;
    bool last_valid;
    struct color last_inner;
    struct color last_outer;
};

static const struct color OFF = {0, 0, 0};
static const struct color GREEN = {.r = 0, .g = 18, .b = 0};
static const struct color BLUE = {.r = 0, .g = 0, .b = 18};
static const struct color CORAL = {.r = 18, .g = 8, .b = 5};
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static const struct color AMBER = {.r = 18, .g = 7, .b = 0};
static const struct color PROFILE_COLORS[] = {
    {.r = 14, .g = 4, .b = 22},
    {.r = 3, .g = 14, .b = 5},
    {.r = 2, .g = 3, .b = 22},
    {.r = 20, .g = 2, .b = 4},
    {.r = 20, .g = 12, .b = 16},
};
#endif

static const struct device *const led_strip = DEVICE_DT_GET(LED_STRIP_NODE);
static const struct gpio_dt_spec led_power = GPIO_DT_SPEC_GET(LED_POWER_NODE, control_gpios);

static struct indicator_state state;
K_MUTEX_DEFINE(state_mutex);
static struct k_work_delayable indicator_work;

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/* ZMK currently exposes split connection state through the active transport. */
extern const struct zmk_split_transport_central *active_transport;
#endif

static bool color_equal(struct color a, struct color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

static bool elapsed_less_than(int64_t now, int64_t since, int64_t duration) {
    return now - since < duration;
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static struct color profile_color(uint8_t profile) {
    return PROFILE_COLORS[MIN(profile, ARRAY_SIZE(PROFILE_COLORS) - 1)];
}
#endif

static uint8_t breath_level(int64_t now) {
    uint32_t ms = now % BREATH_PERIOD_MS;
    uint32_t index = ms * BREATH_STEPS / BREATH_PERIOD_MS;
    uint32_t half = BREATH_STEPS / 2;
    uint32_t up = index < half ? index : BREATH_STEPS - index;
    uint32_t linear = up * 1000 / half;
    uint32_t gamma = linear * linear / 1000;

    return gamma * 255 / 1000;
}

static struct color scale_color(struct color color, uint8_t level) {
    return (struct color){
        .r = (uint16_t)color.r * level / 255,
        .g = (uint16_t)color.g * level / 255,
        .b = (uint16_t)color.b * level / 255,
    };
}

static bool low_blink_on(int64_t now) {
    uint32_t ms = now % LOW_BLINK_PERIOD_MS;
    return ms < 200 || (ms >= 400 && ms < 600);
}

static struct color effect_color(struct effect effect, int64_t now) {
    switch (effect.kind) {
    case EFFECT_SOLID:
        return effect.color;
    case EFFECT_BREATH:
        return scale_color(effect.color, breath_level(now));
    case EFFECT_LOW_BATTERY:
        return low_blink_on(now) ? CORAL : OFF;
    case EFFECT_OFF:
    default:
        return OFF;
    }
}

static bool effect_is_animated(struct effect effect) {
    return effect.kind == EFFECT_BREATH || effect.kind == EFFECT_LOW_BATTERY;
}

static bool battery_at_least(uint8_t threshold) {
    return state.battery_known && state.battery >= threshold;
}

static bool battery_at_most(uint8_t threshold) {
    return state.battery_known && state.battery <= threshold;
}

static void set_charging_locked(bool charging, int64_t now) {
    if (charging == state.charging) {
        return;
    }

    state.charging = charging;
    state.charge_since = now;
    state.pending = true;
}

static void set_battery_locked(uint8_t level, int64_t now) {
    bool was_full = battery_at_least(BATTERY_FULL);
    bool changed = !state.battery_known || state.battery != level;

    state.battery_known = true;
    state.battery = level;

    if (state.charging && level >= BATTERY_FULL && !was_full) {
        state.charge_since = now;
    }
    if (changed) {
        state.pending = true;
    }
}

static void set_peer_connected_locked(bool connected, int64_t now) {
    if (connected == state.peer_connected) {
        return;
    }

    state.peer_connected = connected;
    state.peer_since = now;
    state.pending = true;
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static void set_ble_state_locked(uint8_t profile, bool connected, int64_t now) {
    bool advertising = !connected;

    if (profile != state.ble_profile || connected != state.ble_connected ||
        advertising != state.ble_advertising) {
        state.ble_since = now;
        state.pending = true;
    }

    state.ble_profile = profile;
    state.ble_connected = connected;
    state.ble_advertising = advertising;
}

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
static void set_caps_lock_locked(bool enabled) {
    if (enabled != state.caps_lock) {
        state.caps_lock = enabled;
        state.pending = true;
    }
}
#endif
#endif

static void set_sleeping_locked(bool sleeping) {
    if (sleeping != state.sleeping) {
        state.sleeping = sleeping;
        state.pending = true;
    }
}

static bool read_peer_connected(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (active_transport == NULL || active_transport->api == NULL ||
        active_transport->api->get_status == NULL) {
        return false;
    }

    struct zmk_split_transport_status status = active_transport->api->get_status();
    return status.connections != ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED;
#else
    return zmk_split_bt_peripheral_is_connected();
#endif
}

static void refresh_low_battery_locked(int64_t now) {
    if (!battery_at_most(BATTERY_LOW)) {
        state.low_alert_until = now;
        state.low_next_alert = now;
        return;
    }

    if (now >= state.low_next_alert) {
        state.low_alert_until = now + LOW_ALERT_MS;
        state.low_next_alert = now + LOW_ALERT_MS + LOW_QUIET_MS;
    }
}

static bool low_blinking(int64_t now) {
    return battery_at_most(BATTERY_LOW) && now < state.low_alert_until;
}

static struct effect inner_effect(int64_t now) {
    if (state.charging) {
        if (battery_at_least(BATTERY_FULL)) {
            return elapsed_less_than(now, state.charge_since, NOTICE_MS)
                       ? (struct effect){EFFECT_SOLID, GREEN}
                       : (struct effect){EFFECT_OFF, OFF};
        }
        return (struct effect){EFFECT_BREATH, GREEN};
    }

    if (low_blinking(now)) {
        return (struct effect){EFFECT_LOW_BATTERY, CORAL};
    }

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (!state.peer_connected) {
        return elapsed_less_than(now, state.peer_since, ACTIVITY_MS)
                   ? (struct effect){EFFECT_BREATH, BLUE}
                   : (struct effect){EFFECT_OFF, OFF};
    }
    if (elapsed_less_than(now, state.peer_since, NOTICE_MS)) {
        return (struct effect){EFFECT_SOLID, BLUE};
    }
#endif

    return (struct effect){EFFECT_OFF, OFF};
}

static struct effect outer_effect(int64_t now) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (state.ble_connected && elapsed_less_than(now, state.ble_since, NOTICE_MS)) {
        return (struct effect){EFFECT_SOLID, profile_color(state.ble_profile)};
    }
    if (state.ble_advertising && elapsed_less_than(now, state.ble_since, ACTIVITY_MS)) {
        return (struct effect){EFFECT_BREATH, profile_color(state.ble_profile)};
    }
    if (state.caps_lock) {
        return (struct effect){EFFECT_SOLID, AMBER};
    }
#else
    if (state.peer_connected) {
        return elapsed_less_than(now, state.peer_since, NOTICE_MS)
                   ? (struct effect){EFFECT_SOLID, BLUE}
                   : (struct effect){EFFECT_OFF, OFF};
    }
    if (elapsed_less_than(now, state.peer_since, ACTIVITY_MS)) {
        return (struct effect){EFFECT_BREATH, BLUE};
    }
#endif

    return (struct effect){EFFECT_OFF, OFF};
}

static uint8_t approach_channel(uint8_t current, uint8_t target) {
    if (current < target) {
        return MIN(current + FADE_STEP, target);
    }
    if (current > target) {
        return MAX(current - MIN(current, FADE_STEP), target);
    }
    return current;
}

static struct color approach(struct color current, struct color target) {
    return (struct color){
        .r = approach_channel(current.r, target.r),
        .g = approach_channel(current.g, target.g),
        .b = approach_channel(current.b, target.b),
    };
}

static bool is_animating_locked(void) {
    return state.inner_active || state.outer_active ||
           !color_equal(state.cur_inner, state.tgt_inner) ||
           !color_equal(state.cur_outer, state.tgt_outer);
}

static int render(struct color inner, struct color outer) {
    bool any_on = !color_equal(inner, OFF) || !color_equal(outer, OFF);

    if (!any_on && !state.rail_on) {
        state.last_valid = false;
        return 0;
    }

    if (any_on && !state.rail_on) {
        int rc = gpio_pin_set_dt(&led_power, 1);
        if (rc < 0) {
            LOG_ERR("Failed to enable LED power: %d", rc);
            return rc;
        }
        k_msleep(5);
        state.rail_on = true;
        state.last_valid = false;
    }

    if (!state.last_valid || !color_equal(inner, state.last_inner) ||
        !color_equal(outer, state.last_outer)) {
        struct led_rgb pixels[2] = {
            {.r = inner.r, .g = inner.g, .b = inner.b},
            {.r = outer.r, .g = outer.g, .b = outer.b},
        };
        int rc = led_strip_update_rgb(led_strip, pixels, ARRAY_SIZE(pixels));
        if (rc < 0) {
            LOG_ERR("Failed to update LEDs: %d", rc);
            return rc;
        }
        state.last_inner = inner;
        state.last_outer = outer;
        state.last_valid = true;
    }

    if (!any_on) {
        int rc = gpio_pin_set_dt(&led_power, 0);
        if (rc < 0) {
            LOG_ERR("Failed to disable LED power: %d", rc);
            return rc;
        }
        state.rail_on = false;
        state.last_valid = false;
    }

    return 0;
}

static void indicator_work_handler(struct k_work *work) {
    int64_t now = k_uptime_get();
    struct color inner;
    struct color outer;
    bool animating;
    bool pending;
    bool sleeping;

    k_mutex_lock(&state_mutex, K_FOREVER);
    state.pending = false;

    set_charging_locked(nrf_power_usbregstatus_vbusdet_get(NRF_POWER), now);
    if (!state.battery_known) {
        set_battery_locked(zmk_battery_state_of_charge(), now);
    }
    set_peer_connected_locked(read_peer_connected(), now);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    set_ble_state_locked(zmk_ble_active_profile_index(), zmk_ble_active_profile_is_connected(),
                         now);
#endif

    refresh_low_battery_locked(now);

    if (state.sleeping) {
        state.inner_active = false;
        state.outer_active = false;
        state.tgt_inner = OFF;
        state.tgt_outer = OFF;
    } else {
        struct effect inner_fx = inner_effect(now);
        struct effect outer_fx = outer_effect(now);
        state.inner_active = effect_is_animated(inner_fx);
        state.outer_active = effect_is_animated(outer_fx);
        state.tgt_inner = effect_color(inner_fx, now);
        state.tgt_outer = effect_color(outer_fx, now);
    }

    state.cur_inner = approach(state.cur_inner, state.tgt_inner);
    state.cur_outer = approach(state.cur_outer, state.tgt_outer);
    inner = state.cur_inner;
    outer = state.cur_outer;
    animating = is_animating_locked();
    pending = state.pending;
    sleeping = state.sleeping;
    k_mutex_unlock(&state_mutex);

    render(inner, outer);

    if (sleeping && !animating && !pending) {
        return;
    }

    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &indicator_work,
                                K_MSEC((animating || pending) ? ANIM_INTERVAL_MS
                                                             : IDLE_INTERVAL_MS));
}

static void indicator_kick(void) {
    if (state.initialized) {
        k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &indicator_work, K_NO_WAIT);
    }
}

static int indicator_listener(const zmk_event_t *eh) {
    int64_t now = k_uptime_get();
    bool prefer_usb = false;

    k_mutex_lock(&state_mutex, K_FOREVER);

    const struct zmk_battery_state_changed *battery = as_zmk_battery_state_changed(eh);
    if (battery != NULL) {
        set_battery_locked(battery->state_of_charge, now);
    }

    const struct zmk_split_peripheral_status_changed *split =
        as_zmk_split_peripheral_status_changed(eh);
    if (split != NULL) {
        set_peer_connected_locked(split->connected, now);
    }

    const struct zmk_activity_state_changed *activity = as_zmk_activity_state_changed(eh);
    if (activity != NULL) {
        /* IDLE already stops battery reporting; put the status LEDs on the same
         * lifecycle so their rail and periodic polling also stop while unused.
         */
        set_sleeping_locked(activity->state != ZMK_ACTIVITY_ACTIVE);
    }

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#if IS_ENABLED(CONFIG_ZMK_USB)
    const struct zmk_usb_conn_state_changed *usb = as_zmk_usb_conn_state_changed(eh);
    if (usb != NULL && zmk_usb_is_hid_ready()) {
        prefer_usb = true;
    }
#endif

    const struct zmk_ble_active_profile_changed *ble = as_zmk_ble_active_profile_changed(eh);
    if (ble != NULL) {
        set_ble_state_locked(ble->index, zmk_ble_active_profile_is_connected(), now);
    }

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    const struct zmk_hid_indicators_changed *indicators = as_zmk_hid_indicators_changed(eh);
    if (indicators != NULL) {
        set_caps_lock_locked((indicators->indicators & HID_INDICATOR_CAPS_LOCK) != 0);
    }
#endif
#endif

    k_mutex_unlock(&state_mutex);
    if (prefer_usb) {
        zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_USB);
    }
    indicator_kick();
    return 0;
}

ZMK_LISTENER(cornix_indicator, indicator_listener);
ZMK_SUBSCRIPTION(cornix_indicator, zmk_battery_state_changed);
ZMK_SUBSCRIPTION(cornix_indicator, zmk_split_peripheral_status_changed);
ZMK_SUBSCRIPTION(cornix_indicator, zmk_activity_state_changed);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
ZMK_SUBSCRIPTION(cornix_indicator, zmk_ble_active_profile_changed);
#if IS_ENABLED(CONFIG_ZMK_USB)
ZMK_SUBSCRIPTION(cornix_indicator, zmk_usb_conn_state_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
ZMK_SUBSCRIPTION(cornix_indicator, zmk_hid_indicators_changed);
#endif
#endif

static int cornix_indicator_init(void) {
    if (!device_is_ready(led_strip)) {
        LOG_ERR("LED strip is not ready");
        return -ENODEV;
    }
    if (!gpio_is_ready_dt(&led_power)) {
        LOG_ERR("LED power control is not ready");
        return -ENODEV;
    }
    int rc = gpio_pin_configure_dt(&led_power, GPIO_OUTPUT_INACTIVE);
    if (rc < 0) {
        LOG_ERR("Failed to configure LED power: %d", rc);
        return rc;
    }

    int64_t now = k_uptime_get();
    state = (struct indicator_state){
        .pending = true,
        .ble_advertising = IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL),
        .ble_since = now,
        .peer_since = now,
        .charge_since = now,
        .low_alert_until = now,
        .low_next_alert = now,
    };

    k_work_init_delayable(&indicator_work, indicator_work_handler);
    state.initialized = true;
    k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &indicator_work, K_MSEC(100));
    return 0;
}

SYS_INIT(cornix_indicator_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
