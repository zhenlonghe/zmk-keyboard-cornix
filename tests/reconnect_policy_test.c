#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "boards/shields/cornix_indicator/reconnect_policy.h"

static void test_startup_does_not_schedule(void) {
    struct cornix_reconnect_policy policy = {0};
    assert(cornix_reconnect_policy_update(&policy, false, false, false) ==
           CORNIX_RECONNECT_NONE);
    assert(!policy.armed);
}

static void test_single_link_loss_uses_native_reconnect(void) {
    struct cornix_reconnect_policy policy = {0};
    assert(cornix_reconnect_policy_update(&policy, true, true, false) ==
           CORNIX_RECONNECT_NONE);
    assert(policy.armed);
    assert(cornix_reconnect_policy_update(&policy, true, false, false) ==
           CORNIX_RECONNECT_NONE);
    assert(!policy.pending);
}

static void test_dual_loss_schedules_recovery(void) {
    struct cornix_reconnect_policy policy = {0};
    cornix_reconnect_policy_update(&policy, true, true, false);
    assert(cornix_reconnect_policy_update(&policy, false, false, false) ==
           CORNIX_RECONNECT_SCHEDULE);
    assert(policy.pending);
}

static void test_reconnect_cancels_recovery(void) {
    struct cornix_reconnect_policy policy = {0};
    cornix_reconnect_policy_update(&policy, true, true, false);
    cornix_reconnect_policy_update(&policy, false, false, false);
    assert(cornix_reconnect_policy_update(&policy, true, false, false) ==
           CORNIX_RECONNECT_CANCEL);
    assert(!policy.pending);
}

static void test_expiry_recovers_only_once(void) {
    struct cornix_reconnect_policy policy = {0};
    cornix_reconnect_policy_update(&policy, true, true, false);
    cornix_reconnect_policy_update(&policy, false, false, false);
    assert(cornix_reconnect_policy_expired(&policy, false, false, false));
    assert(!policy.armed);
    assert(!policy.pending);
    assert(!cornix_reconnect_policy_expired(&policy, false, false, false));
}

static void test_sleep_cancels_recovery(void) {
    struct cornix_reconnect_policy policy = {0};
    cornix_reconnect_policy_update(&policy, true, true, false);
    cornix_reconnect_policy_update(&policy, false, false, false);
    assert(cornix_reconnect_policy_update(&policy, false, false, true) ==
           CORNIX_RECONNECT_CANCEL);
    assert(!cornix_reconnect_policy_expired(&policy, false, false, true));
}

int main(void) {
    test_startup_does_not_schedule();
    test_single_link_loss_uses_native_reconnect();
    test_dual_loss_schedules_recovery();
    test_reconnect_cancels_recovery();
    test_expiry_recovers_only_once();
    test_sleep_cancels_recovery();
    puts("reconnect policy tests passed");
    return 0;
}
