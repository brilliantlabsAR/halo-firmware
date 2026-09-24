---
name: flash
description: Flash Halo firmware over BLE OTA (SMP/MCUmgr) and verify the device boots. Use after building to get zephyr.signed.bin onto a device. Covers normal OTA (ota_flash.py) and recovering a device that won't boot, from MCUboot's BLE DFU mode (dfu_flash.py). Wireless app-image updates only — not first-time or bootloader flashing.
---

# Flash Halo firmware over BLE OTA

The scripts live in `alif/applications/halo/tools/` (user-facing home; the
user doc for them is `alif/applications/halo/FLASHING.md`). They are
self-contained: run them with `uv run` from any directory — uv resolves
`brilliant-ble` from PyPI via inline metadata (no brilliant_sdk checkout
needed).

```
uv run alif/applications/halo/tools/ota_flash.py \
    build/halo/zephyr/zephyr.signed.bin --name "Halo AB" --yes
```

Then verify the device came back and the REPL answers (this is what lets the
app self-confirm the test boot):

```
uv run alif/applications/halo/tools/verify.py --name "Halo AB"
```

## Rules

- **Dev kit first, always.** Flash the **Halo Dev Kit** (wired, expendable)
  and verify there before any real Halo unit. Flash a real
  unit **only with explicit user go-ahead for that specific flash**. A
  bricked real device means Hardware Recovery Mode and a filesystem wipe.
- **Always pass `--name`** so you can't flash whatever Halo happens to be
  advertising nearby.
- Flashing takes several minutes (~560 KB at ~384 B/packet) — run it in the
  background and check progress in the output file.

## Test-boot semantics

The default upload marks the image for a **one-shot test boot**: MCUboot
reverts to the previous firmware on the next reboot unless the image is
confirmed. The app self-confirms (`boot_write_img_confirmed()`) after a clean
boot — so a good image sticks and a bad one auto-reverts. Don't use
`--dangerously-auto-confirm` unless the user asks for it. Note the safety net
is not absolute: a half-alive image can self-confirm and still misbehave, which
is exactly why real devices are flashed last.

## Recovering a non-booting device (BLE DFU mode)

MCUboot enters **BLE DFU recovery mode** by itself when it finds no bootable
image, and stays there advertising an SMP server until flashed. A 10 s button
hold through a power-cycle forces the same mode (LED at full brightness).

**`ota_flash.py` cannot flash a device in DFU mode** — `brilliant_ble.connect()`
requires the app's Frame GATT service, which the bootloader does not expose, so
it fails before the upload starts. Use `dfu_flash.py`, which talks SMP directly
and depends only on `bleak`:

```
uv run alif/applications/halo/tools/dfu_flash.py --name "Halo EC"          # slot state only
uv run alif/applications/halo/tools/dfu_flash.py build/halo/zephyr/zephyr.signed.bin \
    --name "Halo EC" --yes
```

Flags mirror `ota_flash.py`. The firmware argument is optional (omit it to read
slot state), and `--retries` defaults to 4 because connects to these devices
drop with `reason=0x98` often. It reports which mode it reached — "app is
running" vs "BOOTLOADER DFU MODE" — and works in both, so prefer it whenever the
device's state is unknown.

Two traps:

- **Entering DFU by button formats `/lfs`**: `main.lua`, user files and BLE bonds
  are erased (`boot_ble_dfu_enter()` calls `halo_file_format()` only for the
  button path). Back `/lfs` up first if it matters. The automatic no-image path
  does not format and has no inactivity timeout; the button path exits after
  2 minutes idle and cold-reboots.
- **Do not detect DFU mode from the advertisement.** The bootloader links the
  same `halo_ble_init()` as the app, so it advertises the same Frame and battery
  UUIDs. Tell them apart by the GATT service table (DFU = one service, SMP only)
  or by console silence (the bootloader is built `CONFIG_LOG=n`).

## Connection gotchas

- After the flash the device reboots; give it ~10 s before verifying.
- The device boots straight into its `main.lua`; if that has a main loop, REPL
  commands time out. `verify.py` handles this by sending a **break signal**
  right after connecting (and a Lua-VM reset when done so the app resumes) —
  do the same in any ad-hoc probing script (`send_break_signal()` /
  `send_reset_signal()`).
- Halo bonds with up to **5 hosts**, so after a one-time initial pairing per
  host no unpair/re-pair dance is needed — even across OTA flashes.
- **Encryption timeout on connect** (macOS `CBErrorDomain Code=15`, "Failed to
  encrypt the connection") means the Mac's stored bond no longer matches the
  device — e.g. a firmware change to bond storage invalidated device-side
  bonds. Fix: re-pair (forget the device in macOS Bluetooth settings, or hold
  the device button ~5 s to clear its bonds), then retry. This needs the user;
  report it rather than retrying in a loop.
- **Every SMP request timing out, while the Lua channel still works**, is a
  stale macOS GATT cache rather than broken firmware — SMP writes go without a
  response, so a stale handle fails silently and the MCUmgr server looks dead
  (seen with the trivial OS group timing out and image-state silent for 150 s).
  Cycle the host adapter: `blueutil -p 0 && blueutil -p 1`. Prefer that over the
  Bluetooth settings pane if the user's keyboard and mouse are on the adapter.
- First-time / bootloader / bricked-device flashing is wired (SE-UART, Alif
  tools): see the wired-flashing appendix in `alif/applications/halo/SETUP.md`.
- To pull the device's persisted `/lfs` logs (post-flash diagnostics), use the
  `logs` skill.
