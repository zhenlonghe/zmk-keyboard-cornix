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

static inline enum cornix_reconnect_action
cornix_reconnect_policy_update(struct cornix_reconnect_policy *policy, bool host_connected,
                               bool peer_connected, bool usb_active, bool deep_sleeping) {
    /* A working USB output means the user is not stranded; never reboot under them. */
    if (deep_sleeping || usb_active) {
        bool was_pending = policy->pending;
        policy->pending = false;
        return was_pending ? CORNIX_RECONNECT_CANCEL : CORNIX_RECONNECT_NONE;
    }

    if (host_connected && peer_connected) {
        bool was_pending = policy->pending;
        policy->armed = true;
        policy->pending = false;
        return was_pending ? CORNIX_RECONNECT_CANCEL : CORNIX_RECONNECT_NONE;
    }

    if (host_connected || peer_connected) {
        bool was_pending = policy->pending;
        policy->pending = false;
        return was_pending ? CORNIX_RECONNECT_CANCEL : CORNIX_RECONNECT_NONE;
    }

    if (policy->armed && !policy->pending) {
        policy->pending = true;
        return CORNIX_RECONNECT_SCHEDULE;
    }

    return CORNIX_RECONNECT_NONE;
}

static inline bool cornix_reconnect_policy_expired(struct cornix_reconnect_policy *policy,
                                                    bool host_connected, bool peer_connected,
                                                    bool usb_active, bool deep_sleeping) {
    if (!policy->armed || !policy->pending || host_connected || peer_connected || usb_active ||
        deep_sleeping) {
        policy->pending = false;
        return false;
    }

    policy->armed = false;
    policy->pending = false;
    return true;
}

#endif
