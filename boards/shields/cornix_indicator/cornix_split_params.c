/*
 * Cornix idle-aware split link parameters (central only).
 *
 * As BLE central the left half cannot use peripheral latency to skip split
 * connection events, so its awake-idle current is dominated by the split
 * interval (CONFIG_ZMK_SPLIT_BLE_PREF_INT, 11.25 ms). While ZMK is idle (no key
 * or encoder activity for CONFIG_ZMK_IDLE_TIMEOUT, 30 s by default) nothing
 * needs that responsiveness, so stretch the link to SLOW_INTERVAL and restore
 * the Kconfig values on the first activity.
 *
 * SLOW_INTERVAL x (SLOW_LATENCY + 1) = 45 ms matches the forced-sync period
 * of the typing-mode parameters: the RC-LFCLK drift budget (see
 * cornix_left_defconfig) and the right half's idle current are unchanged,
 * only the left half wakes ~4x less. Cost: the first right-half keys after an
 * idle period wait up to 45 ms, until the switch back lands (~6 slow events).
 *
 * ZMK only applies the split parameters when it creates the connection and
 * the peripheral never requests its own (BT_GAP_AUTO_UPDATE_CONN_PARAMS=n),
 * so nothing else fights these updates. The left half is the BLE central only
 * on the split link — host links are peripheral-role — so role selects it.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/bluetooth/conn.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/workqueue.h>

LOG_MODULE_REGISTER(cornix_split_params, CONFIG_ZMK_LOG_LEVEL);

#define SLOW_INTERVAL 36 /* x 1.25 ms = 45 ms */
#define SLOW_LATENCY 0
/* Let ZMK finish GATT discovery/subscription on a fresh link first. */
#define RECONNECT_APPLY_DELAY_MS 2000
#define RETRY_DELAY_MS 500
#define MAX_RETRIES 5

static void apply_work_handler(struct k_work *work);

static atomic_t idle;
static uint8_t retries;
static K_WORK_DELAYABLE_DEFINE(apply_work, apply_work_handler);

static struct bt_le_conn_param desired_param(void) {
    if (atomic_get(&idle)) {
        return (struct bt_le_conn_param)BT_LE_CONN_PARAM_INIT(
            SLOW_INTERVAL, SLOW_INTERVAL, SLOW_LATENCY, CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT);
    }
    return (struct bt_le_conn_param)BT_LE_CONN_PARAM_INIT(
        CONFIG_ZMK_SPLIT_BLE_PREF_INT, CONFIG_ZMK_SPLIT_BLE_PREF_INT,
        CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY, CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT);
}

static bool is_split_conn(struct bt_conn *conn, struct bt_conn_info *info) {
    return bt_conn_get_info(conn, info) == 0 && info->role == BT_CONN_ROLE_CENTRAL &&
           info->state == BT_CONN_STATE_CONNECTED;
}

static void schedule_apply(k_timeout_t delay) {
    retries = 0;
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &apply_work, delay);
}

struct apply_ctx {
    struct bt_le_conn_param param;
    bool failed;
};

static void apply_to_conn(struct bt_conn *conn, void *data) {
    struct apply_ctx *ctx = data;
    struct bt_conn_info info;

    if (!is_split_conn(conn, &info)) {
        return;
    }
    if (info.le.interval == ctx->param.interval_max && info.le.latency == ctx->param.latency) {
        return;
    }

    int err = bt_conn_le_param_update(conn, &ctx->param);
    if (err < 0 && err != -EALREADY) {
        LOG_WRN("Split param update to %u/%u failed: %d", ctx->param.interval_max,
                ctx->param.latency, err);
        ctx->failed = true;
    }
}

static void apply_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    struct apply_ctx ctx = {.param = desired_param()};
    bt_conn_foreach(BT_CONN_TYPE_LE, apply_to_conn, &ctx);

    /* Typically -EBUSY/-EIO while another LL procedure is in flight. */
    if (ctx.failed && retries < MAX_RETRIES) {
        retries++;
        k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &apply_work,
                                    K_MSEC(RETRY_DELAY_MS));
    }
}

/* An update requested before an IDLE <-> ACTIVE flip can land after it: the
 * work item saw the old parameters still in place and skipped. Re-check
 * whenever the controller reports new parameters so the link never stays on
 * the slow set while typing.
 */
static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
                             uint16_t timeout) {
    ARG_UNUSED(timeout);
    struct bt_conn_info info;

    if (!is_split_conn(conn, &info)) {
        return;
    }
    LOG_DBG("Split params now %u/%u", interval, latency);

    struct bt_le_conn_param want = desired_param();
    if (interval != want.interval_max || latency != want.latency) {
        schedule_apply(K_NO_WAIT);
    }
}

BT_CONN_CB_DEFINE(cornix_split_params_conn_cb) = {
    .le_param_updated = le_param_updated,
};

static int split_params_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *activity = as_zmk_activity_state_changed(eh);
    if (activity != NULL) {
        /* SLEEP tears the link down anyway; only IDLE <-> ACTIVE matters. */
        if (activity->state == ZMK_ACTIVITY_SLEEP) {
            return 0;
        }
        bool now_idle = activity->state == ZMK_ACTIVITY_IDLE;
        if (atomic_set(&idle, now_idle) != now_idle) {
            schedule_apply(K_NO_WAIT);
        }
        return 0;
    }

    /* A new link starts on the Kconfig (typing-mode) parameters. */
    const struct zmk_split_peripheral_status_changed *split =
        as_zmk_split_peripheral_status_changed(eh);
    if (split != NULL && split->connected && atomic_get(&idle)) {
        schedule_apply(K_MSEC(RECONNECT_APPLY_DELAY_MS));
    }
    return 0;
}

ZMK_LISTENER(cornix_split_params, split_params_listener);
ZMK_SUBSCRIPTION(cornix_split_params, zmk_activity_state_changed);
ZMK_SUBSCRIPTION(cornix_split_params, zmk_split_peripheral_status_changed);
