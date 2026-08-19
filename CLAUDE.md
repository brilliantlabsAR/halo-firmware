# Halo firmware — agent quick reference

Brilliant Labs **Halo** firmware. Zephyr + West, Alif Balletto SoC, board `halo`.
Full environment/toolchain/flasher setup is in `applications/halo/SETUP.md` — this file
is just the things that aren't obvious from the tree.

## Repo topology
- This (`alif`) is **both the West manifest repo and the app repo**. App commits go here
  (remote `brilliantlabsAR/halo-firmware`). The West workspace root is the parent dir
  (`../`, not a git repo); sibling repos are `zephyr`, `modules`, `bootloader`, etc.
- Main app: `applications/halo`. Reusable C/Lua modules: `modules/halo`.

## Build & flash
Use the **`build` and `flash` skills** (`.claude/skills/build`, `.claude/skills/flash`)
— they carry self-contained scripts plus the full procedure and gotchas
(dev-kit-first rule, test-boot semantics, bond pitfalls, VERSION build noise).
Signed OTA payload: `build/halo/zephyr/zephyr.signed.bin` (workspace root).
Wired / first-time / bootloader flashing: the appendix of `SETUP.md`.
User-facing OTA doc: `applications/halo/FLASHING.md` (the flash scripts live
in `applications/halo/tools/`; the flash skill wraps them).

## West branching gotcha
- Branching off the **current HEAD** needs **no `west update`** and won't disturb sibling
  repos or `build/`, *as long as `west.yml` is unchanged*. Make feature branches freely.
- Switching to a branch with a **different `west.yml`** changes pinned revisions and DOES
  require `west update` (this is the real reason branch-switching here is "expensive").

## Boot sequence facts that bite
- `main.lua` runs on the **REPL thread** spawned by `halo_lua_runtime_init()` and starts
  concurrently the moment that's called. Anything that must run before the app (e.g. a boot
  splash) goes **before** runtime init and blocks the main thread (which otherwise just
  `k_sleep(K_FOREVER)`s).
- The **display boots in power-save**. Resume it (`display_resume_handler`) before drawing;
  return it to power-save afterwards so the app brings it up normally. Pan calibration is a
  hardware border-register offset applied to the whole framebuffer — don't add it to draw
  coordinates.
- The speaker (**MAX98357A**) inits at POST_KERNEL and is ready before `main()` — no startup
  delay is needed (sound playback retries until ready, logging the latency).

## Device REPL & `main.lua` facts that bite
- **REPL `print()` goes to BLE, not the UART console** — to generate console traffic on
  demand, call something that logs device-side. And a **newline terminates a REPL
  command**, so device-side helpers must be one-liners.
- The device boots into `main.lua`; when probing a live device, **send a break signal
  before REPL commands and a VM reset after** so the app resumes
  (`applications/halo/tools/verify.py` is the reference pattern).
- **`main.lua` is the installed application**: it lives in `/lfs` and survives OTA
  flashes. Anything that overwrites or deletes it must save/restore it
  (`preserve_main_lua()` in the tests). A `main.lua` with non-UTF-8 bytes can't be read
  back over the REPL (string chunk reads die decoding); `send_remove_signal()` still works.
- **Dev-kit vs production IMU mounting differs**: `direction()`'s axis remap targets
  production hardware, so the dev kit reading ~89° off level is expected, not a fault.
  Verify orientation work on a real unit.

## Tools
- `modules/halo/tools/gen_boot_logo.py` — encodes a 16-colour PNG to an LZ4 boot-logo blob
  (`src/boot_logo_data.c`).
