#ifndef CORNIX_RECONNECT_POLICY_H
#define CORNIX_RECONNECT_POLICY_H

#include <stdbool.h>

enum cornix_reconnect_action {
    CORNIX_RECONNECT_NONE,
    CORNIX_RECONNECT_SCHEDULE,
    CORNIX_RECONNECT_CANCEL,
};

struct cornix_reconnect_policy {
    bool armed;
    bool pending;
};

struct cornix_reconnect_inputs {
    bool host_connected;
    bool peer_connected;
    bool usb_active;
    bool deep_sleeping;
    bool recent_activity;
};

static inline enum cornix_reconnect_action
cornix_reconnect_cancel_pending(struct cornix_reconnect_policy *policy) {
    bool was_pending = policy->pending;
    policy->pending = false;
    return was_pending ? CORNIX_RECONNECT_CANCEL : CORNIX_RECONNECT_NONE;
}

static inline enum cornix_reconnect_action
cornix_reconnect_policy_update(struct cornix_reconnect_policy *policy,
                               const struct cornix_reconnect_inputs *in) {
    /* Never reboot a sleeping keyboard or one with another working output path. */
    if (in->deep_sleeping || in->usb_active) {
        return cornix_reconnect_cancel_pending(policy);
    }

    if (in->host_connected && in->peer_connected) {
        policy->armed = true;
        return cornix_reconnect_cancel_pending(policy);
    }

    if (in->host_connected || in->peer_connected) {
        return cornix_reconnect_cancel_pending(policy);
    }

    /* Activity gates new scheduling only: an idle dual drop stays armed until
     * the next key press, while a recovery already pending keeps its timer even
     * if the activity window lapses during the delay.
     */
    if (!in->recent_activity) {
        return CORNIX_RECONNECT_NONE;
    }

    if (policy->armed && !policy->pending) {
        policy->pending = true;
        return CORNIX_RECONNECT_SCHEDULE;
    }

    return CORNIX_RECONNECT_NONE;
}

/* Activity gates scheduling only. Re-checking it here would silently drop a
 * recovery whose delay outlived the activity window, so a pending recovery
 * fires even if the user went idle while it waited.
 */
static inline bool cornix_reconnect_policy_expired(struct cornix_reconnect_policy *policy,
                                                   const struct cornix_reconnect_inputs *in) {
    if (!policy->armed || !policy->pending || in->host_connected || in->peer_connected ||
        in->usb_active || in->deep_sleeping) {
        policy->pending = false;
        return false;
    }

    policy->armed = false;
    policy->pending = false;
    return true;
}

#endif
