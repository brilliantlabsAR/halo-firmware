---
name: flash
description: Flash Halo firmware over BLE OTA (SMP/MCUmgr) and verify the device boots. Use after building to get zephyr.signed.bin onto a device. Wireless app-image updates only — not first-time or bootloader flashing.
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
- First-time / bootloader / bricked-device flashing is wired (SE-UART, Alif
  tools): see the wired-flashing appendix in `alif/applications/halo/SETUP.md`.
- To pull the device's persisted `/lfs` logs (post-flash diagnostics), use the
  `logs` skill.
