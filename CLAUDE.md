# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

Community ZMK firmware for the Cornix split ergonomic keyboard (nRF52840, Ebyte E73-2G4M08S1C module). It is a ZMK *module* (`zephyr/module.yml` sets `board_root: .` and `snippet_root: .`) providing custom board definitions and shields, plus the user keymap under `config/`.

`firmware/` is **gitignored** — it is the local output directory for `build-docker.sh`. Released `.uf2` files come from GitHub Actions artifacts / tagged releases, not from the repo tree.

## Commands

### Build (local, Docker — primary method)

```bash
./build-docker.sh [all|left|right] [--update]
```

Clones/caches ZMK under `.build/zmk-docker/`, builds `cornix_left` + `cornix_right` with the `cornix_indicator` shield, and copies `.uf2` outputs into `firmware/`. `--update` refreshes ZMK and west deps first. It auto-starts OrbStack/Docker Desktop on macOS. Note it builds ZMK from `main`, not the SHA pinned in `config/west.yml` — CI is the source of truth for reproducible builds.

### Build (Nix + Justfile)

`flake.nix` provides the Zephyr SDK devshell; the `Justfile` drives west builds (`just list`, `just build <expr>` filters `build.yaml` targets, `just draw <keyboard>` renders keymap SVGs, `just test <path>` runs ZMK native_posix snapshot tests). **The Justfile expects `ZMK_LIB_PREFIX` and a `config2/` dir that this repo does not track** — `build-docker.sh` is the reliable local path.

### Tests

The reconnect policy is pure C logic, unit-tested on the host (same command as CI):

```bash
cc -Wall -Wextra -Werror -I. -o /tmp/policy_test tests/reconnect_policy_test.c && /tmp/policy_test
```

This is the only test that runs in CI. There are no ZMK snapshot tests checked in.

### CI

- `.github/workflows/build.yml` — runs the policy test, then builds the `build.yaml` matrix via ZMK's reusable `build-user-config.yml`. Triggered on pushes touching `boards/`, `config/`, `tests/`.
- `.github/workflows/release_with_tag.yml` — on `v*.*` tags, builds the same matrix and publishes a zip. A tag containing any letter (e.g. `v1.2-beta`) is marked prerelease.

**The ZMK revision is hardcoded in three places** — `config/west.yml`, `build.yml`, and `release_with_tag.yml`. Bump all three together.

## Architecture

### Boards (`boards/jzf/cornix/`)

Three targets, all nRF52840 on the E73 module, sharing `cornix.dtsi` → (`cornix-pinctrl.dtsi`, `cornix-layouts.dtsi`, `cornix_sensors.dtsi`, `nrf_e73.dtsi`):

- **`cornix_left`** — left half as BLE/USB central (no-dongle setup).
- **`cornix_ph_left`** — left half as peripheral, for dongle setups.
- **`cornix_right`** — right half, always peripheral.

`cornix_left.dts` and `cornix_ph_left.dts` are identical one-line includes of `cornix_left_common.dtsi`; **the only difference between them is the `_defconfig` + `Kconfig.defconfig` role settings**.

Hardware facts that constrain edits:

- **No-SoftDevice flash layout.** Partitions in `nrf_e73.dtsi`: 4K stub, 844K code at `0x1000`, 128K storage, 48K boot. The board targets bake this in; the dongle/`nice_nano` targets in `build.yaml` get it via the `nrf52840-nosd` snippet. `.uf2` files flash directly.
- **RC-only LFCLK** — the E73 module has no 32.768 kHz crystal. Keep `CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC` + `_RC_CALIBRATION`; never switch to XTAL.
- **Matrix**: `zmk,kscan-gpio-matrix`, col2row, 4 rows × 7 cols per half. The two halves use *different, swapped* row/col pins (see each `.dts`); the right half applies `col-offset = <7>` onto the 14-column `default_transform`.
- `pinmux.c` drives P0.05 low at init to enable the onboard charger (`CONFIG_BOARD_CORNIX_CHARGER`).
- Encoders (`alps,ec11`) are declared disabled in `cornix_sensors.dtsi` and enabled per-half in the `.dts`.
- Radio tuning convention: **both** sides run `CONFIG_BT_CTLR_TX_PWR_PLUS_8` and disable `CONFIG_BT_CTLR_PHY_2M`, so every link (split and host) runs 1M PHY for its ~4 dB sensitivity advantage at range. The defconfigs carry the rationale in comments — read them before retuning.

### Shields (`boards/shields/`)

- **`cornix_indicator`** — holds all custom C in the repo; despite the name it also implements the reliability features:
  - `cornix_indicator.c` (~750 lines) — RGB status LED engine (battery / charging / BLE profile / peer link / caps-lock), LED power-rail gating via `zmk,ext-power-generic`, a `k_work_delayable` render loop, and the BLE dual-disconnect recovery driver. Subscribes to ZMK events and links against **ZMK-internal symbols** (`zmk/split/transport/central.h`, `zmk/workqueue.h`, …) — this is why the ZMK revision is pinned.
  - `cornix_recovery.c` — firmware-halt self-recovery: overrides `k_sys_fatal_error_handler` to cold-reboot instead of halting, a 30 s hardware watchdog armed lazily on the first feed from the indicator loop, and a crash counter retained in **GPREGRET2** (GPREGRET1 belongs to the UF2 bootloader). 3 consecutive crashes → `bootmode_set(BOOTLOADER)`; beyond that → lock-free System OFF to save the battery.
  - `reconnect_policy.h` — header-only pure state machine (arm / schedule / cancel) for the dual-disconnect fallback. **Keep it free of Zephyr dependencies** — `tests/reconnect_policy_test.c` compiles it standalone.
  - `cornix_indicator.overlay` enables `spi3` + `ws2812` + `led_power` (all declared `disabled` in the board dtsi) and exports the `status-ws2812` / `status-led-power` aliases the C code resolves via `DT_ALIAS`.
- **`cornix_dongle_adapter`** — matrix/BLE glue for custom dongle boards (`CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS=2`, 7 BLE connections).
- **`cornix_dongle_eyelash`** — example display shield for a dongle board lacking `zephyr,display`.

**Non-obvious build wiring:** ZMK shields cannot contribute C sources, so `boards/jzf/cornix/CMakeLists.txt` compiles `../../shields/cornix_indicator/*.c` guarded by `CONFIG_SHIELD_CORNIX_INDICATOR`. Adding a new source file to the indicator shield means editing that board-level `CMakeLists.txt`, and the indicator code only ever builds for the `cornix_*` boards — never for dongle targets.

### Config (`config/`)

- `cornix.keymap` — 50-key layout, 5 layers (Base/Number/Symbol/Nav/FN), home-row mods (`hml`/`hmr`, balanced flavor, cross-hand `hold-trigger-key-positions`), and the copy/paste combos. `cornix42.keymap` is the 42-key variant.
- **The cross-half soft-off combo is *not* in the keymap** — the `csoff` behavior and `soft_off_combo` (key positions 41+46, `split-peripheral-off-on-press`) live in `boards/jzf/cornix/cornix.dtsi`, and the matching System-OFF wake wiring (`zmk,gpio-key-wakeup-trigger`, driving col 4) lives in each half's `.dts`. The comments there explain why the wake column must not be a combo key's column — read them before touching soft off.
- `config/includes/cornix50.h` documents position names (LT0/RM3/…) for the 50-key mapping but **is not `#include`d by any keymap** — it is reference material, not live code.
- `west.yml` — pins ZMK, `zmk-helpers`, and `zmk-dongle-display` by SHA. Bump deliberately (see CI note above).

### Design docs

Reliability-feature specs and plans live under `docs/superpowers/specs/` and `docs/superpowers/plans/` (BLE reconnect recovery, firmware-halt recovery). Read these before touching recovery or reconnect code — they carry the failure analysis the code comments only summarize.

## Conventions

- `README.md` and `README_zh.md` are parallel English/Chinese docs with the same heading structure. Update both when changing user-facing documentation.
- Config files carry Chinese comments in places; match the surrounding language.
- Tuning changes to `.conf`/`_defconfig` are expected to come with a comment explaining the measurement or symptom that motivated them — follow that pattern.

## Agent skills

- **Issue tracker** — GitHub Issues on `zhenlonghe/zmk-keyboard-cornix` via the `gh` CLI. See `docs/agents/issue-tracker.md`.
- **Triage labels** — `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`. See `docs/agents/triage-labels.md`.
- **Domain docs** — single-context layout (`CONTEXT.md` + `docs/adr/` at the repo root). Neither exists yet; per `docs/agents/domain.md`, proceed silently rather than flagging their absence.
