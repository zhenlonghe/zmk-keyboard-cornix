/*
 * SPDX-License-Identifier: MIT
 *
 * Copy of ZMK's behavior_soft_off.c with one addition: a blocking white
 * double-blink on the status LEDs immediately before zmk_pm_soft_off(), so
 * each half visibly confirms it is actually powering off. A custom behavior
 * is the only reliable seam for this: zmk_pm_soft_off() suspends every
 * device (including the LED rail and SPI bus) before sys_poweroff(), and
 * neither ZMK nor Zephyr raises any pre-poweroff event or hook.
 */

#define DT_DRV_COMPAT zmk_behavior_cornix_soft_off

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/pm.h>
#include <zmk/behavior.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Strong implementation lives in cornix_indicator.c; builds without the
 * indicator shield fall back to this no-op and keep soft off working.
 */
__weak void cornix_indicator_soft_off_flash(void) {}

struct behavior_cornix_soft_off_config {
    bool split_peripheral_turn_off_on_press;
    uint32_t hold_time_ms;
};

struct behavior_cornix_soft_off_data {
    uint32_t press_start;
};

#define IS_SPLIT_PERIPHERAL                                                                        \
    (IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_cornix_soft_off_data *data = dev->data;
    const struct behavior_cornix_soft_off_config *config = dev->config;

    if (IS_SPLIT_PERIPHERAL && config->split_peripheral_turn_off_on_press) {
        /* Runs synchronously in the split RX thread; the command has already
         * arrived, so blocking here for the blink costs nothing.
         */
        cornix_indicator_soft_off_flash();
        zmk_pm_soft_off();
    } else {
        data->press_start = k_uptime_get();
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_cornix_soft_off_data *data = dev->data;
    const struct behavior_cornix_soft_off_config *config = dev->config;

    if (config->hold_time_ms == 0) {
        LOG_DBG("No hold time set, triggering soft off");
        cornix_indicator_soft_off_flash();
        zmk_pm_soft_off();
    } else {
        uint32_t hold_time = k_uptime_get() - data->press_start;

        if (hold_time > config->hold_time_ms) {
            if (IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) {
                k_sleep(K_MSEC(100));
            }
            cornix_indicator_soft_off_flash();
            zmk_pm_soft_off();
        } else {
            LOG_INF("Not triggering soft off: held for %d and hold time is %d", hold_time,
                    config->hold_time_ms);
        }
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_cornix_soft_off_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

#define BCSO_INST(n)                                                                               \
    static const struct behavior_cornix_soft_off_config bcso_config_##n = {                        \
        .hold_time_ms = DT_INST_PROP_OR(n, hold_time_ms, 0),                                       \
        .split_peripheral_turn_off_on_press =                                                      \
            DT_INST_PROP_OR(n, split_peripheral_off_on_press, false),                              \
    };                                                                                             \
    static struct behavior_cornix_soft_off_data bcso_data_##n = {};                                \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, &bcso_data_##n, &bcso_config_##n, POST_KERNEL,          \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_cornix_soft_off_driver_api);

DT_INST_FOREACH_STATUS_OKAY(BCSO_INST)
