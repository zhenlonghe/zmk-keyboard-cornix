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
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/fatal.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/retention/bootmode.h>
#include <zephyr/sys/reboot.h>

#include <hal/nrf_power.h>

#include "cornix_recovery.h"

LOG_MODULE_REGISTER(cornix_recovery, CONFIG_ZMK_LOG_LEVEL);

#define CRASH_COUNT_GPREGRET_INDEX 1U
#define CRASH_COUNT_MAGIC 0xC0U
#define CRASH_COUNT_MAGIC_MASK 0xF0U
#define CRASH_COUNT_VALUE_MASK 0x0FU
#define CRASH_COUNT_LIMIT 3U
#define CRASH_COUNT_STABLE_MS 60000

/* GPREGRET1 is reserved for the UF2 bootloader magic. GPREGRET2 is otherwise
 * unused on Cornix and survives software and watchdog resets without a flash
 * write. Keep the fatal path lock-free because the kernel may already be
 * corrupted or holding a lock.
 */
static uint8_t crash_count_get(void) {
    uint8_t value = (uint8_t)nrf_power_gpregret_get(NRF_POWER, CRASH_COUNT_GPREGRET_INDEX);

    return (value & CRASH_COUNT_MAGIC_MASK) == CRASH_COUNT_MAGIC
               ? value & CRASH_COUNT_VALUE_MASK
               : 0;
}

static void crash_count_set(uint8_t count) {
    uint8_t value = count == 0 ? 0 : CRASH_COUNT_MAGIC | MIN(count, CRASH_COUNT_VALUE_MASK);

    nrf_power_gpregret_set(NRF_POWER, CRASH_COUNT_GPREGRET_INDEX, value);
}

static uint8_t crash_count_increment(void) {
    uint8_t count = crash_count_get();

    if (count < CRASH_COUNT_VALUE_MASK) {
        count++;
    }
    crash_count_set(count);
    return count;
}

static void crash_count_clear_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (crash_count_get() != 0) {
        crash_count_set(0);
        LOG_INF("Crash counter cleared after stable runtime");
    }
}

K_WORK_DELAYABLE_DEFINE(crash_count_clear_work, crash_count_clear_work_handler);

/* The BLE stack asserts via k_oops() (CONFIG_BT_ASSERT default), and Zephyr's
 * default handler halts with interrupts locked until a manual power cycle.
 * Reboot instead so a crashed half rejoins the split within seconds.
 */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf) {
    ARG_UNUSED(esf);

    uint8_t crash_count = crash_count_increment();

    LOG_PANIC();
    LOG_ERR("Fatal error %u; recovery reboot %u/%u", reason, crash_count, CRASH_COUNT_LIMIT);

    /* A deterministic fatal before this module's SYS_INIT would reboot forever
     * without reaching the crash-loop check there. bootmode_set() may take a
     * mutex, so it cannot run here; strictly-greater-than gives the init path
     * one boot to claim the bootloader before a lock-free System OFF breaks
     * the loop and preserves the battery.
     */
    if (crash_count > CRASH_COUNT_LIMIT) {
        nrf_power_system_off(NRF_POWER);
    }
    sys_reboot(SYS_REBOOT_COLD);
}

/* Watchdog and CPU-lockup resets never run the fatal handler, and a
 * deterministic early crash never reaches the APPLICATION init below — so the
 * reset cause is sampled at EARLY init (register access only), where the count
 * can also break the loop with a lock-free System OFF. Strictly-greater-than
 * leaves the APPLICATION path one boot to claim the bootloader first.
 */
static int cornix_crash_count_early_init(void) {
    uint32_t reset_cause = 0;

    if (hwinfo_get_reset_cause(&reset_cause) == 0) {
        /* RESETREAS is cumulative on nRF52, so clear it after sampling. */
        hwinfo_clear_reset_cause();

        if ((reset_cause & (RESET_WATCHDOG | RESET_CPU_LOCKUP)) != 0) {
            uint8_t crash_count = crash_count_increment();

            if (crash_count > CRASH_COUNT_LIMIT) {
                nrf_power_system_off(NRF_POWER);
            }
        }
    }

    return 0;
}

SYS_INIT(cornix_crash_count_early_init, EARLY, 0);

static int cornix_crash_recovery_init(void) {
    uint8_t crash_count = crash_count_get();

    if (crash_count != 0) {
        LOG_ERR("Recovery boot %u/%u after fatal/watchdog/lockup", crash_count,
                CRASH_COUNT_LIMIT);
    }

    if (crash_count >= CRASH_COUNT_LIMIT) {
        LOG_ERR("Crash loop detected; entering bootloader");

        int rc = bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);
        if (rc == 0) {
            crash_count_set(0);
            sys_reboot(SYS_REBOOT_WARM);
        }

        /* Never halt: a keyboard that keeps trying beats a bricked one that
         * drains its battery spinning in arch_system_halt().
         */
        LOG_ERR("Failed to enter bootloader: %d; continuing normal boot", rc);
        crash_count_set(0);
    }

    if (crash_count != 0) {
        k_work_schedule(&crash_count_clear_work, K_MSEC(CRASH_COUNT_STABLE_MS));
    }

    return 0;
}

SYS_INIT(cornix_crash_recovery_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

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
