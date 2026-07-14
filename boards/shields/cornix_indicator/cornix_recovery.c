/*
 * Cornix firmware halt recovery.
 *
 * The reconnect policy in reconnect_policy.h only helps while the firmware is
 * still running. These two layers cover the halves wedging outright:
 *  - a fatal-error handler that cold-reboots instead of halting forever, and
 *  - a hardware watchdog fed from the indicator work loop.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>

#include "cornix_recovery.h"

LOG_MODULE_REGISTER(cornix_recovery, CONFIG_ZMK_LOG_LEVEL);

/* The BLE stack asserts via k_oops() (CONFIG_BT_ASSERT default), and Zephyr's
 * default handler halts with interrupts locked until a manual power cycle.
 * Reboot instead so a crashed half rejoins the split within seconds.
 */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf) {
    ARG_UNUSED(esf);

    LOG_PANIC();
    LOG_ERR("Fatal error %u; rebooting for recovery", reason);
    sys_reboot(SYS_REBOOT_COLD);
}

#if IS_ENABLED(CONFIG_WATCHDOG)

#include <zephyr/drivers/watchdog.h>

/* Six times the indicator's awake-idle poll interval. */
#define RECOVERY_WDT_TIMEOUT_MS 30000

static const struct device *const wdt = DEVICE_DT_GET(DT_NODELABEL(wdt0));

static bool wdt_attempted;
static int wdt_channel = -1;

/* Armed lazily from the first feed so a keyboard whose feeder never starts
 * (e.g. LED init failure keeps the indicator loop from running) degrades to
 * "no watchdog" instead of a 30-second reboot loop.
 */
static void arm_watchdog(void) {
    if (!device_is_ready(wdt)) {
        LOG_ERR("Watchdog is not ready");
        return;
    }

    const struct wdt_timeout_cfg cfg = {
        .window = {.min = 0, .max = RECOVERY_WDT_TIMEOUT_MS},
        .flags = WDT_FLAG_RESET_SOC,
    };

    int channel = wdt_install_timeout(wdt, &cfg);
    if (channel < 0) {
        LOG_ERR("Failed to install watchdog timeout: %d", channel);
        return;
    }

    /* Keep counting while the CPU idles so stalls are caught between key
     * presses. Deep sleep is System OFF, which stops LFCLK and with it the
     * watchdog; waking from it resets the SoC and re-arms through here.
     */
    int rc = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
    if (rc < 0) {
        LOG_ERR("Failed to start watchdog: %d", rc);
        return;
    }

    wdt_channel = channel;
    LOG_INF("Recovery watchdog armed (%d ms)", RECOVERY_WDT_TIMEOUT_MS);
}

void cornix_recovery_feed(void) {
    if (!wdt_attempted) {
        wdt_attempted = true;
        arm_watchdog();
    }

    if (wdt_channel >= 0) {
        wdt_feed(wdt, wdt_channel);
    }
}

#else

void cornix_recovery_feed(void) {}

#endif /* IS_ENABLED(CONFIG_WATCHDOG) */
