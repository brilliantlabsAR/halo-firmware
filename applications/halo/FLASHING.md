# Flashing Halo over the air (BLE OTA)

This is how firmware gets onto a Halo: wirelessly, over Bluetooth LE, using
the MCUmgr/SMP DFU service built into the shipped firmware. No cable, no
dev kit, no vendor tools.

## What you need

- A **signed app image** — either:
  - `build/halo/zephyr/zephyr.signed.bin` from your own build
    (see [`SETUP.md`](SETUP.md)), or
  - `halo-firmware-<version>-release.signed.bin` downloaded from the
    [GitHub Releases](../../../../releases) page (no toolchain needed).
- [`uv`](https://docs.astral.sh/uv/) installed. That's the only dependency —
  the flasher script is self-contained and pulls the `brilliant-ble` package
  from PyPI on first run.
- A Halo that is powered on, advertising, and **paired to your computer**
  (pair once per host; Halo keeps up to 5 bonds, and bonds survive OTA
  updates).

## Flash

```bash
uv run alif/applications/halo/tools/ota_flash.py \
    build/halo/zephyr/zephyr.signed.bin --name "Halo XX"
```

`--name` is the device's advertised Bluetooth name (shown in your OS
Bluetooth settings, e.g. `Halo 3F`). Always pass it so you can't flash
whichever Halo happens to be advertising nearby. The upload takes a few
minutes (~600 KB at ~384 bytes per packet); the script prints progress and
reboots the device into the new image when done.

Then confirm the device came back and the firmware is responsive:

```bash
uv run alif/applications/halo/tools/verify.py --name "Halo XX"
```

## Safety net: the one-shot test boot

By default the uploaded image is marked for a **one-shot test boot**:

- MCUboot boots the new image once.
- If it boots cleanly, the app confirms itself
  (`boot_write_img_confirmed()`) and the update sticks.
- If it crashes before confirming, the **next reboot automatically reverts**
  to the previous firmware.

So a bad build generally costs you a reboot, not a device. Hold the button
to power-cycle if the device hangs, and you're back on the old firmware.
`--dangerously-auto-confirm` skips this protection — don't use it unless you
have a specific reason.

The net is not absolute: an image that boots far enough to self-confirm but
then misbehaves is kept. Test changes incrementally.

## Troubleshooting

- **Device not found**: check it's advertising (not connected to another
  host — Halo holds one connection at a time), and that the `--name` matches
  exactly.
- **"Failed to encrypt the connection" / macOS `CBErrorDomain Code=15`**:
  your computer's stored bond no longer matches the device. Forget the
  device in your OS Bluetooth settings (or hold the Halo button ~5 s to open
  pairing), re-pair, and retry.
- **macOS "Writing is not permitted"** during upload: toggle your Mac's
  Bluetooth off and on — it's a stale macOS GATT cache, not a device
  problem.
- **After flashing, give the device ~10 s** to reboot before reconnecting.

## What OTA cannot do

OTA updates the **application image only**. The MCUboot bootloader itself
and first-time factory programming use Alif's wired SE-UART tooling — see
the wired-flashing appendix in [`SETUP.md`](SETUP.md). You will not need
that unless you are working with bare boards or have defeated the test-boot
safety net.

Pairing, button gestures, and LED states are described in
[`BUTTON_LED_GUIDE.md`](BUTTON_LED_GUIDE.md) and [`PAIRING.md`](../../PAIRING.md).
