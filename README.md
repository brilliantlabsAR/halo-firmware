# Halo firmware

Firmware for **Halo**, Brilliant Labs' AI glasses — a Zephyr RTOS
application for the Alif Balletto B1 (Cortex-M55 + BLE 5.3 + Ethos-U55
NPU), with a Lua runtime for on-device apps, LE-Audio-style voice
pipelines with on-device echo cancellation, a MIPI-DSI display, and
wireless (BLE OTA) firmware updates.

**Halo owners can build modified firmware and flash it to their own device
over the air.** No cable, no dev kit, no vendor tooling — the device's OTA
update path plus MCUboot's test-boot/auto-revert safety net make
self-built firmware a supported workflow.

## Quick start

**Just want the latest firmware?** Download the signed image from
[Releases](../../releases) and follow
[`applications/halo/FLASHING.md`](applications/halo/FLASHING.md).

**Want to build it yourself?** The last step below needs a working Zephyr
toolchain — Python 3.10+, CMake, Ninja, and the Zephyr SDK —
**[`applications/halo/SETUP.md`](applications/halo/SETUP.md)** walks through
installing each (about ten minutes). This is a west workspace: you don't
clone this repo directly; `west init` assembles a workspace around it.

```bash
pip install west          # in a venv — see the setup guide
mkdir halo-firmware && cd halo-firmware
west init -m https://github.com/brilliantlabsAR/halo-firmware
west update
west build --sysbuild -b halo alif/applications/halo -p
```

The signed OTA image lands at `build/halo/zephyr/zephyr.signed.bin`. Full
environment setup (toolchain, per-OS prerequisites) and the explanation of
the workspace layout: **[`applications/halo/SETUP.md`](applications/halo/SETUP.md)**.
Flashing: **[`applications/halo/FLASHING.md`](applications/halo/FLASHING.md)**.

**Rather not install a toolchain?** You can build inside the CI container
image instead — the recipe is in
[`SETUP.md`](applications/halo/SETUP.md#building-in-the-ci-container-no-local-toolchain).

## Repository tour

| Path | What it is |
|------|-----------|
| `applications/halo/` | The Halo application (boot, BLE, Lua runtime, audio, display) |
| `modules/halo/` | Reusable Halo C modules + Lua bindings (AEC, codecs, sensors, BLE services) |
| `modules/lua/`, `modules/canvas/` | Lua 5.4 and the display/canvas + font stack |
| `boards/arm/halo/` | The `halo` Zephyr board definition |
| `drivers/`, `subsys/` | Device drivers and subsystems (Halo-specific + inherited Alif SDK) |
| `samples/halo/` | Small single-peripheral samples (mic, display, sensors, …) |
| `west.yml` | The west manifest pinning Zephyr, HALs, and MCUboot |
| `applications/halo/PROTOCOL.md` | BLE protocol reference for host apps |

Key docs: [`SETUP.md`](applications/halo/SETUP.md) ·
[`FLASHING.md`](applications/halo/FLASHING.md) ·
[`PROTOCOL.md`](applications/halo/PROTOCOL.md) ·
[`BLE_SERVICES.md`](applications/halo/BLE_SERVICES.md) ·
[`LUA_RUNTIME.md`](applications/halo/LUA_RUNTIME.md) ·
[`BUTTON_LED_GUIDE.md`](applications/halo/BUTTON_LED_GUIDE.md) ·
[`PAIRING.md`](PAIRING.md) ·
[`PM_SLEEP.md`](applications/halo/PM_SLEEP.md)

## A note on update safety

OTA uploads default to a **one-shot test boot**: if your image fails to
boot, the device automatically reverts to the previous firmware on the
next reboot. Images are signed with the standard MCUboot development key —
deliberately, so owners can build and flash their own firmware; signing is
not an anti-tamper control on this device, and access control lives in the
BLE pairing layer ([`PAIRING.md`](PAIRING.md)).

## Licensing

This repository is a fork of Alif Semiconductor's Zephyr SDK with the Halo
application added, and it deliberately carries **more than one license**:

- **Brilliant Labs code** (`applications/halo/`, `modules/halo/`, the
  `halo` board, most Halo drivers) — Apache-2.0, per file headers.
- **Alif Semiconductor SDK code** (`samples/`, `subsys/`, most remaining
  `drivers/`, build scaffolding) — the
  [Alif Semiconductor Software License Agreement](License.txt), which
  permits source redistribution but restricts use to Alif silicon and
  forbids relicensing under copyleft terms.
- **Vendored third-party code** — Lua 5.4 (MIT), the Dogica pixel font by
  Roberto Mocci (SIL OFL-1.1, rasterized into the display font headers), the
  Adafruit GFX font structures (BSD-3-Clause), Bosch Sensortec BMA5xx driver
  (BSD-3-Clause), Arm Ethos-U/model code (Apache-2.0), each under its own
  header.

The per-file header governs. The repository as a whole is therefore *not*
uniformly Apache-2.0, and cannot be relicensed as such.

## Contributing

CI builds every PR inside a prebaked container image (see
[`.github/CI.md`](.github/CI.md)). Firmware releases are cut with the
`build-and-release` workflow. Note that a debug/release build rewrites
`applications/halo/VERSION`'s `EXTRAVERSION` line as a side effect — don't
include that hunk in commits.
