/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/devicetree.h>

/* P0.05 is the charger enable line: drive it low to enable charging.
 * Only built for the cornix_* boards (boards/jzf/cornix/CMakeLists.txt), so no
 * board guard is needed. The old `#if CONFIG_BOARD_CORNIX` guard never matched
 * after the Zephyr 4 board split (only CONFIG_BOARD_CORNIX_LEFT/_RIGHT/_PH_LEFT
 * exist), which silently left P0.05 floating. GPIO_OUTPUT_LOW avoids a
 * high glitch between configure and set. */
static int pinmux_cornix_init(void) {
    const struct device *p0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
#if IS_ENABLED(CONFIG_BOARD_CORNIX_CHARGER)
    gpio_pin_configure(p0, 5, GPIO_OUTPUT_LOW);
#else
    gpio_pin_configure(p0, 5, GPIO_INPUT);
#endif
    return 0;
}

SYS_INIT(pinmux_cornix_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
