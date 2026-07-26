#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "boards/shields/cornix_indicator/reconnect_policy.h"

#define INPUTS(...) (&(struct cornix_reconnect_inputs){__VA_ARGS__})

static void test_startup_does_not_schedule(void) {
    struct cornix_reconnect_policy policy = {0};
    assert(cornix_reconnect_policy_update(&policy, INPUTS(.recent_activity = true)) ==
           CORNIX_RECONNECT_NONE);
    assert(!policy.armed);
}

static void test_single_link_loss_uses_native_reconnect(void) {
    struct cornix_reconnect_policy policy = {0};
    assert(cornix_reconnect_policy_update(&policy, INPUTS(.host_connected = true,
                                                          .peer_connected = true,
                                                          .recent_activity = true)) ==
           CORNIX_RECONNECT_NONE);
    assert(policy.armed);
    assert(cornix_reconnect_policy_update(&policy, INPUTS(.host_connected = true,
                                                          .recent_activity = true)) ==
           CORNIX_RECONNECT_NONE);
    assert(!policy.pending);
}

static void test_dual_loss_schedules_recovery(void) {
    struct cornix_reconnect_policy policy = {0};
    cornix_reconnect_policy_update(&policy, INPUTS(.host_connected = true, .peer_connected = true,
                                                   .recent_activity = true));
    assert(cornix_reconnect_policy_update(&policy, INPUTS(.recent_activity = true)) ==
           CORNIX_RECONNECT_SCHEDULE);
    assert(policy.pending);
}

static void test_idle_dual_loss_waits_for_activity(void) {
    struct cornix_reconnect_policy policy = {0};
    cornix_reconnect_policy_update(&policy, INPUTS(.host_connected = true, .peer_connected = true));
    assert(policy.armed);
    assert(cornix_reconnect_policy_update(&policy, INPUTS()) == CORNIX_RECONNECT_NONE);
    assert(!policy.pending);
    assert(cornix_reconnect_policy_update(&policy, INPUTS(.recent_activity = true)) ==
           CORNIX_RECONNECT_SCHEDULE);
}

static void test_reconnect_cancels_recovery(void) {
    struct cornix_reconnect_policy policy = {0};
    cornix_reconnect_policy_update(&policy, INPUTS(.host_connected = true, .peer_connected = true,
                                                   .recent_activity = true));
    cornix_reconnect_policy_update(&policy, INPUTS(.recent_activity = true));
    assert(cornix_reconnect_policy_update(&policy, INPUTS(.host_connected = true,
                                                          .recent_activity = true)) ==
           CORNIX_RECONNECT_CANCEL);
    assert(!policy.pending);
}

static void test_expiry_recovers_only_once(void) {
    struct cornix_reconnect_policy policy = {0};
    cornix_reconnect_policy_update(&policy, INPUTS(.host_connected = true, .peer_connected = true,
                                                   .recent_activity = true));
    cornix_reconnect_policy_update(&policy, INPUTS(.recent_activity = true));
    assert(cornix_reconnect_policy_expired(&policy, INPUTS(.recent_activity = true)));
    assert(!policy.armed);
    assert(!policy.pending);
    assert(!cornix_reconnect_policy_expired(&policy, INPUTS(.recent_activity = true)));
}

static void test_stale_activity_keeps_pending_recovery(void) {
    /* The indicator loop keeps polling update() while a recovery waits; the
     * activity window lapsing during the delay must not cancel the timer.
     */
    struct cornix_reconnect_policy policy = {0};
    cornix_reconnect_policy_update(&policy, INPUTS(.host_connected = true, .peer_connected = true,
                                                   .recent_activity = true));
    cornix_reconnect_policy_update(&policy, INPUTS(.recent_activity = true));
    assert(cornix_reconnect_policy_update(&policy, INPUTS()) == CORNIX_RECONNECT_NONE);
    assert(policy.pending);
}

static void test_expiry_ignores_stale_activity(void) {
    /* Activity gates scheduling only: a recovery scheduled while the user was
     * active must still fire when the delay outlives the activity window.
     */
    struct cornix_reconnect_policy policy = {0};
    cornix_reconnect_policy_update(&policy, INPUTS(.host_connected = true, .peer_connected = true,
                                                   .recent_activity = true));
    cornix_reconnect_policy_update(&policy, INPUTS(.recent_activity = true));
    assert(cornix_reconnect_policy_expired(&policy, INPUTS()));
}

static void test_sleep_cancels_recovery(void) {
    struct cornix_reconnect_policy policy = {0};
    cornix_reconnect_policy_update(&policy, INPUTS(.host_connected = true, .peer_connected = true,
                                                   .recent_activity = true));
    cornix_reconnect_policy_update(&policy, INPUTS(.recent_activity = true));
    assert(cornix_reconnect_policy_update(&policy, INPUTS(.deep_sleeping = true,
                                                          .recent_activity = true)) ==
           CORNIX_RECONNECT_CANCEL);
    assert(!cornix_reconnect_policy_expired(&policy, INPUTS(.deep_sleeping = true,
                                                            .recent_activity = true)));
}

static void test_usb_active_does_not_schedule(void) {
    struct cornix_reconnect_policy policy = {0};
    cornix_reconnect_policy_update(&policy, INPUTS(.host_connected = true, .peer_connected = true,
                                                   .recent_activity = true));
    assert(cornix_reconnect_policy_update(&policy, INPUTS(.usb_active = true,
                                                          .recent_activity = true)) ==
           CORNIX_RECONNECT_NONE);
    assert(!policy.pending);
    assert(policy.armed);
}

static void test_usb_active_blocks_expiry(void) {
    struct cornix_reconnect_policy policy = {0};
    cornix_reconnect_policy_update(&policy, INPUTS(.host_connected = true, .peer_connected = true,
                                                   .recent_activity = true));
    cornix_reconnect_policy_update(&policy, INPUTS(.recent_activity = true));
    assert(policy.pending);
    assert(!cornix_reconnect_policy_expired(&policy, INPUTS(.usb_active = true,
                                                            .recent_activity = true)));
    assert(!policy.pending);
}

int main(void) {
    test_startup_does_not_schedule();
    test_single_link_loss_uses_native_reconnect();
    test_dual_loss_schedules_recovery();
    test_idle_dual_loss_waits_for_activity();
    test_reconnect_cancels_recovery();
    test_expiry_recovers_only_once();
    test_stale_activity_keeps_pending_recovery();
    test_expiry_ignores_stale_activity();
    test_sleep_cancels_recovery();
    test_usb_active_does_not_schedule();
    test_usb_active_blocks_expiry();
    puts("reconnect policy tests passed");
    return 0;
}
