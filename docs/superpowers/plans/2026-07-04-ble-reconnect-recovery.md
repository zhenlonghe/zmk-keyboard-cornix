# BLE Reconnect Recovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a tested, one-shot cold-reboot fallback when the Cornix left half loses both the Mac and right half for five seconds, while increasing split-link supervision tolerance to two seconds.

**Architecture:** Keep ZMK's native advertising and split scanning as the first recovery path. Add a dependency-free header-only policy state machine, integrate it with the existing indicator listener under the existing state mutex, and invoke Zephyr cold reboot only after the policy expires. The policy arms only after both links were healthy, cancels on either-link recovery or deep sleep, and disarms before reboot.

**Tech Stack:** C11 host tests, Zephyr delayed work, ZMK events, Kconfig, Docker-based ZMK builds.

---

### Task 1: Reconnect policy state machine

**Files:**
- Create: `tests/reconnect_policy_test.c`
- Create: `boards/shields/cornix_indicator/reconnect_policy.h`

- [ ] **Step 1: Write the failing host test**

```c
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
```

- [ ] **Step 2: Run the test and verify RED**

Run:

```bash
cc -std=c11 -Wall -Wextra -Werror -I. tests/reconnect_policy_test.c -o /tmp/cornix-reconnect-policy-test
```

Expected: compilation fails because `reconnect_policy.h` does not exist.

- [ ] **Step 3: Add the minimal policy implementation**

```c
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
                               bool peer_connected, bool deep_sleeping) {
    if (deep_sleeping) {
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
                                                    bool deep_sleeping) {
    if (!policy->armed || !policy->pending || host_connected || peer_connected || deep_sleeping) {
        policy->pending = false;
        return false;
    }

    policy->armed = false;
    policy->pending = false;
    return true;
}

#endif
```

- [ ] **Step 4: Run the host test and verify GREEN**

Run:

```bash
cc -std=c11 -Wall -Wextra -Werror -I. tests/reconnect_policy_test.c -o /tmp/cornix-reconnect-policy-test
/tmp/cornix-reconnect-policy-test
```

Expected: `reconnect policy tests passed`.

### Task 2: Integrate the five-second fallback on the left half

**Files:**
- Modify: `boards/shields/cornix_indicator/cornix_indicator.c`

- [ ] **Step 1: Add integration includes, state and delayed work**

Add the Zephyr reboot include, policy include, delay constant, `deep_sleeping` state field, and central-only globals:

```c
#include <zephyr/sys/reboot.h>

#include "reconnect_policy.h"

#define RECONNECT_RECOVERY_DELAY_MS 5000

bool sleeping;
bool deep_sleeping;

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static struct cornix_reconnect_policy reconnect_policy;
static struct k_work_delayable reconnect_recovery_work;
#endif
```

- [ ] **Step 2: Add policy-to-workqueue integration**

Add these central-only helpers after `read_peer_connected()`:

```c
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static void update_reconnect_recovery_locked(void) {
    enum cornix_reconnect_action action = cornix_reconnect_policy_update(
        &reconnect_policy, state.ble_connected, state.peer_connected, state.deep_sleeping);

    if (!state.initialized) {
        return;
    }

    if (action == CORNIX_RECONNECT_SCHEDULE) {
        k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &reconnect_recovery_work,
                                    K_MSEC(RECONNECT_RECOVERY_DELAY_MS));
    } else if (action == CORNIX_RECONNECT_CANCEL) {
        k_work_cancel_delayable(&reconnect_recovery_work);
    }
}

static void reconnect_recovery_work_handler(struct k_work *work) {
    bool should_reboot;

    k_mutex_lock(&state_mutex, K_FOREVER);
    should_reboot = cornix_reconnect_policy_expired(
        &reconnect_policy, state.ble_connected, state.peer_connected, state.deep_sleeping);
    k_mutex_unlock(&state_mutex);

    if (should_reboot) {
        LOG_WRN("Both BLE links remained disconnected; rebooting for recovery");
        sys_reboot(SYS_REBOOT_COLD);
    }
}
#endif
```

- [ ] **Step 3: Feed state transitions into the policy**

After processing the activity event, preserve LED idle behavior while tracking deep sleep separately:

```c
if (activity != NULL) {
    set_sleeping_locked(activity->state != ZMK_ACTIVITY_ACTIVE);
    state.deep_sleeping = activity->state == ZMK_ACTIVITY_SLEEP;
}
```

Immediately before releasing `state_mutex` in `indicator_listener()`, add:

```c
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
update_reconnect_recovery_locked();
#endif
```

After the periodic calls to `set_peer_connected_locked()` and `set_ble_state_locked()` in `indicator_work_handler()`, add the same guarded call so polled state also drives recovery.

- [ ] **Step 4: Initialize recovery work before listeners can schedule it**

In `cornix_indicator_init()`, before `state.initialized = true`, add:

```c
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
k_work_init_delayable(&reconnect_recovery_work, reconnect_recovery_work_handler);
#endif
```

- [ ] **Step 5: Re-run host tests**

Run:

```bash
cc -std=c11 -Wall -Wextra -Werror -I. tests/reconnect_policy_test.c -o /tmp/cornix-reconnect-policy-test
/tmp/cornix-reconnect-policy-test
```

Expected: `reconnect policy tests passed`.

### Task 3: Increase split tolerance and verify firmware

**Files:**
- Modify: `boards/jzf/cornix/cornix_left_defconfig`
- Verify: `build-docker.sh`
- Verify: `build.yaml`

- [ ] **Step 1: Set the split supervision timeout to two seconds**

Replace:

```conf
CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT=100
```

with:

```conf
# Allow brief radio scheduling stalls without delaying genuine link-loss detection too much.
CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT=200
```

- [ ] **Step 2: Build left and right firmware**

Run:

```bash
./build-docker.sh all
```

Expected: exit 0 and both `firmware/cornix_left_central_usb_ble_nosd.uf2` and `firmware/cornix_right_peripheral_nosd.uf2` are produced.

- [ ] **Step 3: Build the receiver matrix entry**

Run:

```bash
docker run --rm \
  -v "$PWD:/config:ro" \
  -v "$PWD/.build/zmk-docker:/work" \
  -v /private/tmp/cornix-zmk-build-20260701/zmk-dongle-display:/dongle-display:ro \
  -w /work/zmk/app \
  zmkfirmware/zmk-build-arm:stable \
  west build -p auto -d build/cornix-dongle -b nice_nano/nrf52840/zmk -- \
  -DSHIELD="cornix_dongle_adapter cornix_dongle_eyelash dongle_display" \
  -DSNIPPET=nrf52840-nosd \
  -DZMK_CONFIG=/config/config \
  '-DZMK_EXTRA_MODULES=/config;/dongle-display'
```

Expected: exit 0 and `/work/zmk/app/build/cornix-dongle/zephyr/zmk.uf2` exists inside the mounted build workspace.

- [ ] **Step 4: Check only intended files changed**

Run:

```bash
git diff --check
git status --short
```

Expected: no whitespace errors; pre-existing `README.md` and `config/cornix.keymap` remain untouched.

- [ ] **Step 5: Commit the implementation files only**

Run:

```bash
git add boards/jzf/cornix/cornix_left_defconfig \
  boards/shields/cornix_indicator/cornix_indicator.c \
  boards/shields/cornix_indicator/reconnect_policy.h \
  tests/reconnect_policy_test.c \
  docs/superpowers/plans/2026-07-04-ble-reconnect-recovery.md
git commit -m "fix(cornix): recover stalled ble links"
```

Expected: one commit containing only the reconnect implementation, tests, timeout adjustment, and this plan.
