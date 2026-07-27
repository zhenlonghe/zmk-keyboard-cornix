# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

Community ZMK firmware for the Cornix split ergonomic keyboard (nRF52840, E73 module). It is a ZMK *module* (`zephyr/module.yml`) providing custom board definitions and shields, plus the user keymap config. Firmware artifacts (`.uf2`) are checked into `firmware/`.

## Commands

### Build (local, Docker — primary method)

```bash
./build-docker.sh [all|left|right] [--update]
```

Clones/caches ZMK under `.build/zmk-docker/`, builds with the `cornix_indicator` shield, and copies `.uf2` outputs into `firmware/`. `--update` refreshes ZMK and west deps first.

### Build (Nix + Justfile)

`flake.nix` provides the Zephyr SDK devshell; the `Justfile` drives west builds (`just list`, `just build <expr>` filters `build.yaml` targets, `just draw <keyboard>` renders keymap SVGs). Note: the Justfile expects `ZMK_LIB_PREFIX` and a `config2/` dir that this repo does not track — `build-docker.sh` is the reliable local path.

### CI

GitHub Actions (`.github/workflows/build.yml`) builds the matrix defined in `build.yaml` using ZMK's `build-user-config.yml`, pinned to the same ZMK revision as `config/west.yml`.

### Tests

The reconnect policy is pure C logic, unit-tested on the host (same command as CI):

```bash
cc -Wall -Wextra -Werror -I. -o /tmp/policy_test tests/reconnect_policy_test.c && /tmp/policy_test
```

## Architecture

### Boards (`boards/jzf/cornix/`)

Three build targets share common `.dtsi` files (`cornix.dtsi`, `nrf_e73.dtsi`, `cornix_left_common.dtsi`, ...):

- **`cornix_left`** — left half as BLE/USB central (no-dongle setup).
- **`cornix_ph_left`** — left half as peripheral, for dongle setups.
- **`cornix_right`** — right half, always peripheral.

All targets use a **no-SoftDevice flash layout** (`nrf52840-nosd` snippet); `.uf2` files flash directly. The E73 module has RC-only LFCLK (no 32 kHz crystal).

### Shields (`boards/shields/`)

- **`cornix_indicator`** — contains all custom C code in the repo; despite the name it also implements the reliability features:
  - `cornix_indicator.c` — RGB status LED engine (battery/charging/connection/caps-lock), LED power-rail gating, and the work loop that feeds the watchdog. Links against ZMK-internal symbols, which is why the ZMK revision is pinned.
  - `cornix_recovery.c` — firmware-halt self-recovery: overrides Zephyr's fatal handler to cold-reboot, 30 s hardware watchdog, and a retained crash-loop counter (3 consecutive crashes → bootloader).
  - `reconnect_policy.h` — header-only pure state machine for the BLE dual-disconnect recovery fallback (arm/schedule/cancel decisions). This is what `tests/reconnect_policy_test.c` covers; keep it free of Zephyr dependencies.
- **`cornix_dongle_adapter`** — matrix/BLE glue for custom dongle boards.
- **`cornix_dongle_eyelash`** — example display shield for a dongle board lacking `zephyr,display`.

### Config (`config/`)

- `cornix.keymap` — 50-key layout, 5 layers (Base/Number/Symbol/Nav/FN), home-row mods, combos (copy/paste, cross-half soft-off). `cornix42.keymap` is the 42-key variant. Key position macros in `config/includes/cornix54.h`.
- `west.yml` — pins ZMK and module revisions by SHA. **Bump deliberately**: the indicator shield uses ZMK-internal symbols, and the pin must stay in sync with the revision hardcoded in `.github/workflows/build.yml`.

### Design docs

Reliability-feature specs/plans live under `docs/superpowers/specs/` and `docs/superpowers/plans/` (BLE reconnect recovery, firmware-halt recovery). Read these before touching recovery/reconnect code.

## Agent skills

### Issue tracker

Issues are tracked in this repo's GitHub Issues (`zhenlonghe/zmk-keyboard-cornix`), via the `gh` CLI. See `docs/agents/issue-tracker.md`.

### Triage labels

Default label vocabulary: `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context layout: `CONTEXT.md` + `docs/adr/` at the repo root. See `docs/agents/domain.md`.
