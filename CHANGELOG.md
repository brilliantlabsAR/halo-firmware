# Changelog

All notable changes to the Halo firmware are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versions are tagged `MAJOR.MINOR.PATCH` (no `v` prefix) and each release
carries the signed OTA images (`X.Y.Z.bin` release build, `X.Y.Z-debug.bin`
debug build). Versions before 0.8.0 predate this changelog, and releases up
to and including 0.8.8 were cut in a private development repository — their
release pages and tags are not publicly reachable.

## [Unreleased]

### Added

- Echo cancellation: the near-end gate also allows for speaker distortion
  (`gate_kappa_hf`, `gate_hf_hz`, `gate_hf_ratio` in
  `frame.microphone.aec_tune`). Some speakers turn a loud sibilant into
  broadband sound at the microphone, and where a reply restarted on one the
  gate took it for the wearer and let the echo through for over a second. In
  a replay of a worn unit with such a speaker, at microphone gain 4, the gate
  was open on echo 2% of the time instead of 27%, and 10.4 dB of echo was
  removed instead of 6.8.
- Echo cancellation: the double-talk test that pauses filter adaptation
  follows each unit's own echo level (`dtd_mult`, `dtd_init`;
  `diag('stats').dtd_thr`). It used to wait until the wearer was about 50
  times louder than the echo, so the filter kept adapting to the wearer's
  voice and the suppressor then removed more of it. Across the worn and desk
  replays the wearer's voice is kept 0.1 to 0.9 dB better, and real double
  talk 0.4 to 0.8 dB better on worn units, with no loss of echo removal
  beyond 0.2 dB. A strongly coupled unit still converges from a cold start,
  and when the echo path changes mid-reply (the glasses re-seated, a
  stronger coupling) the threshold falls back to the old one until the
  filter has re-adapted, within about 2 s (`diag('stats').dtd_escapes`
  counts these).

### Changed

- The microphone's default gain is 1 (was 0): `frame.microphone.start{}`
  without `gain=`, the LE Audio source and the AICS gain's initial value. A
  saved gain (`frame.microphone.gain()` or AICS) still takes precedence.
  Gain 1 is the echo canceller's reference gain.
- With echo cancellation on, every near-end gate threshold
  (`gate_kappa`, `gate_kappa_hf`, `gate_absfloor`, `gate_edge_abs` in
  `frame.microphone.aec_tune`) is expressed at microphone gain 1, and the
  firmware scales it by the effective gain over gain 1 (x2.0 at gain 3,
  x2.5 at gain 4). The canceller's internal mic-versus-speaker power
  thresholds (the double-talk test and the divergence guard) scale with the
  square of that factor. So the canceller decides the same at any gain, a
  value set with `aec_tune` means the same at every gain, and the gain only
  matters through clipping. This follows the gain actually applied
  (`start{gain=}`, a saved gain, `frame.microphone.gain()` or the LE Audio
  gain control), and `diag('stats').mic_gain_scale` reports the factor.
  Before, gain 4 made the gate open on echo (in a replay of one worn unit,
  echo removed fell from 18 to 6 dB and the gate was open on echo 23% of the
  time). A gain change during playback also carries the echo filter over,
  so cancellation holds through it instead of dropping by about 10 dB until
  the filter re-adapted. The calibration harness replays each sitting at
  its effective gain.
- Migrating `aec_tune` lines: on 0.8.18 the gate values (`gate_kappa`,
  `gate_absfloor`, `gate_edge_abs`) were in the units of whatever
  microphone gain was in use. A value tuned at gain g must be divided by
  f = (g + 1) / 2 for g >= 0 (0.5 at gain 0, 1 at gain 1, 2.0 at gain 3,
  2.5 at gain 4) to give the gain-1 value; values tuned at gain 1 carry
  over unchanged. For example the interim gain-4 line
  `aec_tune{gate_kappa=1.25, gate_absfloor=2.25, gate_edge_abs=1.5}`
  becomes 0.5 / 0.9 / 0.6, the 0.8.18 defaults: drop it and use the new
  defaults. Also note the default microphone gain is now 1 (above): an
  app that called `start{}` without `gain=` and relied on gain 0 records
  about 6 dB louder; pass `gain=0` to keep the old level.
- Echo cancellation defaults re-tuned for microphone gain 3 to 4 on worn
  units: `gate_kappa` 0.46 (was 0.5, and now a gain-1 value), `sup_beta`
  1.0 (was 1.25), `cap_lo_gcap` 0.6 (was 0.5), `steady_gcap` 0.15 (was
  0.25), `gate_hang_ms` 1800 (was 1200). In replays of three worn sittings
  of two units at gain 3 and 4 against 0.8.18 at the same gain: on the unit
  whose echo is hard to cancel (two sittings), 10.3 dB of echo removed
  instead of 6.7 to 7.4 and the gate never open on echo (was 24 to 30% of
  the time) on one, 18.4 dB instead of 5.7 at gain 4 on the other, with the
  wearer's voice kept within 0.15 dB of 0.8.18 or better; on the other unit, 13.2 dB removed
  (0.8.18 at its gain 1: 13.8) with the wearer kept 0.9 dB better. Desk
  sittings: echo removed within 0.7 dB of 0.8.18 or better, the gate never
  open on echo.

### Fixed

- The worn calibration harness (`tests/aec/calib`) stops with "device
  storage full: N KB free, need M KB" when `/lfs` cannot hold a recording.
  Before, a full filesystem silently cut the capture short, and the harness
  retried it as a Bluetooth drop until its retries ran out. It now checks
  for room before each recording and checks every write of the capture.

## [0.8.18] - 2026-10-05

### Added

- `frame.microphone.aec_tune()` reads and sets the echo canceller's
  barge-in tunables at runtime: the residual suppressor, the onset duck
  (depth, length, re-arm hold-off), the near-end gate and the filter step
  size. `aec_tune{key=value}` checks every key and applies all of them or
  none, and `aec_tune('defaults')` restores the built-in values. Values
  apply to every session but are not saved: each boot starts from the
  defaults. PROTOCOL.md lists the keys with their units, ranges and
  tradeoffs (#65).
- `applications/halo/tests/aec/calib`, a worn echo-cancellation
  calibration: in one sitting of about 11 minutes it records the wearer
  and the glasses' own echo, replays them on the Mac through this tree's
  canceller to pick `aec_tune` settings, checks the pick on the glasses
  against the defaults, and writes a report with a ready-to-paste
  `frame.microphone.aec_tune{...}` line when the pick holds up there
  (otherwise it says to keep the defaults). See "Worn calibration" in
  `tests/aec/README.md` (#67).

### Changed

- With echo cancellation on, loud speaker echo no longer opens the
  microphone's playback ceiling for most of a reply. The detector that
  lifts the ceiling when the wearer talks mistook the echo of a unit with
  strong speaker-to-mic coupling for the wearer (80% of a reply on a desk
  unit), so the echo came through at full strength. It now measures only
  below 1 kHz, where the canceller works best and most of the voice is,
  with a larger echo allowance: on that unit it no longer opens on echo,
  and echo removal rose from 11 to 17 dB. From 312 to 750 Hz the ceiling
  is now -6 dB instead of -12 dB, so a wearer the detector misses keeps more
  of their voice; a wearer quieter at the microphone than the echo is
  missed while the reply plays. The residual suppressor is slightly
  lighter. New `frame.microphone.aec_tune()` keys `gate_band_hz`,
  `cap_split_hz` and `cap_lo_gcap` control the new behaviour; PROTOCOL.md
  lists the new defaults and how to restore the old ones (#66).
- With echo cancellation on, the playback ceiling is -20 dB instead of
  -12 dB from 1.6 kHz up, where most of the echo the canceller leaves
  behind sits: on two desk units echo removal at 1.6-3.4 kHz rose from
  14-15 to 21-22 dB. Like the rest of the ceiling it lifts when the wearer
  is detected, so it only touches a wearer the detector misses, and their
  voice below 1.6 kHz is not affected. New `frame.microphone.aec_tune()`
  keys `cap_hi_split_hz` (0 turns it off) and `cap_hi_gcap` (#68).
- The AEC loopback, ERLE and interleave test scripts pass `aec=true` or
  `aec=false` to `frame.microphone.start{}` to match the pass under test,
  and the local ERLE test dumps the full `diag('stats')` table on AEC-on
  passes (#61).

### Fixed

- With echo cancellation on, the microphone is no longer muted for about
  a second at the start of a speaker session that follows an earlier one.
  The canceller's onset duck (and its fail-safe for an unreliable speaker
  reference) was frozen when the previous session closed and resumed with
  the next `frame.speaker.start()`, `start{aec=true}` or
  `frame.microphone.aec(true)`, pulling the wearer's voice down by about
  34 dB for up to 1.4 s. Each new session now starts clean, and
  `frame.microphone.aec(true)` no longer resets the canceller from the Lua
  thread while the microphone thread is using it. While the canceller
  refills its reference history (session start, re-anchor) it no longer
  mistakes the not-yet-cancelled echo for the wearer talking, which used to
  let that echo through for about a second (#62).
- Negative microphone gains (`frame.microphone.gain()`, `start{gain=}`,
  AICS) now lower the level in steps that mirror the positive ones: -1 is
  6 dB below 0, -3 is 12 dB below, -10 is 20.8 dB below. They used to drop
  the level by 31-51 dB, with -1 the quietest and -10 the loudest, so any
  negative gain made the microphone effectively silent. Gains 0 and above
  are unchanged. PROTOCOL.md and BLE_SERVICES.md now describe the real
  step sizes; a step is not 1 dB (#63).
- With echo cancellation on, the wearer is no longer cut out after every
  pause in a long reply. The canceller's onset duck (about 34 dB for 0.4 s,
  then easing out) re-armed whenever the speaker audio had been silent for
  160 ms, so every word or sentence gap in a reply ducked the microphone
  again. It now re-arms only after 1 s of speaker silence, which is a new
  reply rather than a pause in one, and it no longer holds down a wearer
  who is already talking once the canceller has adapted. Steady-state echo
  removal is unchanged (#64).

### Documentation

- PROTOCOL.md describes how a break (`0x03`) reaches Lua: it is raised once
  and can be caught by a `pcall`, so scripts with long-running loops must
  re-raise it to hand control back to the REPL. Includes the re-raise
  pattern and what a break stops regardless (#60).

## [0.8.17] - 2026-10-03

### Added

- `frame.speaker.stats([reset])` reports what happened to speaker audio at
  each stage: bytes received and rejected over BLE, frames decoded, concealed,
  muted or discarded, and blocks the amplifier played or threw away. A reply
  that loses audio shows where it was lost. `stats(true)` starts a new
  counting window (#52).

### Changed

- `frame.speaker.start()` on a running stream with the same format and
  `budget` updates `volume` and `gain` in place instead of restarting the
  stream, so the audio already inside the device keeps playing. Apps that
  resend their speaker settings before each reply no longer lose up to a few
  hundred milliseconds of audio. A format or `budget` change still restarts.
  `frame.speaker.stats()` counts these as `updates` (#54).

### Fixed

- `frame.speaker.start()` with an invalid argument raises its error without
  stopping a stream that is already playing. It used to stop the stream
  first, so a bad value (silently, under `pcall`) left the speaker off (#53).
- Speaker audio streamed in writes that are not whole LC3 frames (for
  example MTU-sized chunks) plays correctly. The partial frame at the end of
  each write used to be discarded, shifting every frame after it: the stream
  decoded as noise and then muted. PCM writes with an odd byte count are
  handled the same way (#55).
- `frame.speaker.stop()` discards speaker audio still buffered from BLE, as
  does a Lua VM reset. That audio used to play at the start of the next
  stream, delaying it, and a backlog over about 2 s filled the buffer so
  later writes were refused. Audio written after `stop()` is still kept for
  the next `start()`. `frame.speaker.stats()` reports the discarded bytes
  as `ble_flushed_bytes` (#56).
- A speaker stream running when the device enters standby pauses and picks
  up where it left off on wake. Audio that arrived during standby used to be
  fed to the stopped speaker and lost (`frames_write_failed` in
  `frame.speaker.stats()`) (#57).
- Speaker audio no longer loses a few frames when it arrives faster than
  real time after a quiet moment (the start of a reply, or a backlog
  catching up). The amplifier driver's block queue was one slot smaller
  than its block pool, so a burst while it was playing silence overflowed
  the queue (`frames_write_failed` in `frame.speaker.stats()`) (#58).

## [0.8.16] - 2026-10-03

### Changed

- `halo_realloc` grows or shrinks a block in place when it lives in the
  internal heap and the requested region is internal or `AUTO`, instead of
  always allocating, copying and freeing. Lua table and buffer growth is
  10–18 % faster. Blocks in external SRAM take the existing path, so `AUTO`
  allocations still return to the internal heap first (#26).
- `frame.camera.read()` returns `nil` until a captured frame is ready, and
  `frame.camera.capture()` makes the previous frame unreadable straight away
  (#34).
- `frame.display.bitmap()` rejects `x_scale` / `y_scale` larger than the
  display (#30).
- The battery level filter keeps its state in fixed point. The reported
  level now tracks the gauge to within about a point instead of sitting about
  four points away, and the low/critical callbacks fire at 20 % and 10 % of
  the gauge reading (#50, fixes #37).

### Fixed

- `file:read(n)` returns up to `n` bytes instead of at most 512, and
  `file:read()` returns lines longer than 512 characters whole (#24).
- `frame.time.utc()` and `frame.time.date()` stay correct past 2^32 ms
  (49.7 days) of uptime after the last sync (#25).
- `frame.file.remove_all()` and the remove-all control signal empty
  directories with more than 32 entries (#28, fixes #27).
- An ANCS response with a TLV length near 0xFFFF is no longer treated as
  complete (#29).
- `frame.display.line()`, `circle()` and `bitmap()` with very large
  coordinates, radii or scale factors return promptly instead of looping in
  C, and a two-point `polygon()` draws its segment (#30).
- The callback setters in `frame.button`, `frame.imu`, `frame.bluetooth` and
  `frame.compression` register the function argument when extra arguments are
  passed (#31).
- An mcumgr `os reset` forces a cold BLE start on the next boot, like the
  other reboot paths (#33).
- The Battery Level CCC write is confirmed before the initial notification is
  sent (#35).
- The watchdog-fired flag is cleared once `main()` has acted on it (#32).

## [0.8.15] - 2026-10-02

### Added

- `tests/halo_mainboard` has a `factory clockstream [seconds|stop]` console
  command. It prints one `FT clk` line per LF second with the 64-bit CPU
  cycle and LPRTC counts, so a station can fit both against host time and
  measure the HFXO and LFXO error on a board. It runs only on request: the
  boot run is unchanged, and `factory run` / `factory <test>` stop it (#48).

### Changed

- The SE device config's crystal trims are calibrated: `HFXO_CAP_CTRL` 5,
  `LFXO_CAP_CTRL` 45 and `LFXO_GM_CTRL` 15, matching the production
  flashing station (#47).

## [0.8.14] - 2026-10-01

### Added

- `tests/halo_mainboard`, a factory test for bare Halo main PCBs at the
  flashing station. It is written raw at 0x80000000 over SE-UART (about
  117 KB), runs at boot, and reports machine-parseable results on the
  console as `FT <test> PASS|FAIL|SKIP <detail>` lines ending with
  `FT DONE`. It covers the Secure Enclave and EUI, the crystals, BLE
  advertising, RAM, MRAM, battery sense, the accelerometer, the
  magnetometer, the microphone, and the display/camera connector parts.
  Releases publish it as `halo-mainboard-test-X.Y.Z.bin`, and PR CI
  builds it (#45).

### Fixed

- `VERSION.txt` on releases records the build date again. The date format's
  quotes were escaped wrongly inside the CI container script, so `date`
  failed and the field was left empty (#44).

## [0.8.13] - 2026-09-29

### Added

- `tools/dfu_flash.py` flashes an app image to a device sitting in the
  bootloader's BLE DFU mode, for recovering a unit whose app won't boot.
  `FLASHING.md` documents the recovery flow (#39).

### Changed

- Releases now also carry the MCUboot bootloader
  (`halo-bootloader-X.Y.Z.bin`), the factory test firmware
  (`halo-factory-test-X.Y.Z.bin`, a raw image for wired flashing) and a
  `SHA256SUMS` file. Debug and release builds share one bootloader, so the
  `-debug`/`-release` bootloader pair is replaced by a single file.
  `VERSION.txt` and the pre-release notes now record the commit actually
  built instead of the workflow's dispatch ref, plus the digest of the CI
  build image and the compiler version, so a release can be rebuilt
  byte-for-byte (#41).
- `libmpix` is pinned in `west.yml` by commit SHA rather than by its
  `v1.2.0` tag, like every other project (#42). Builds are unchanged.

### Fixed

- Ship mode (the level-3 long press) now checks that the shutdown device is
  ready before formatting the filesystem, checks the format's result,
  invalidates the in-RAM BLE bond table straight after the wipe, and reboots
  if the PMIC shutdown returns (#38). Before, a failure after the format left
  the device running with `/lfs` erased and a stale bond table.
- The factory test firmware (`tests/halo`) builds again against the
  current board definition. PR CI now builds it too (#41).

### Documentation

- `FLASHING.md` notes that a host's stale GATT cache can make working
  firmware look dead (#39).

## [0.8.12] - 2026-09-22

### Changed

- `require()` no longer caches modules in `package.loaded`; it loads and runs
  the file on every call, as the original Frame firmware and the Halo
  emulator do. Apps are started by `require()`-ing their main module, so
  with the cache (added in 0.8.8, #260) an app that exited its main loop
  cleanly could not be started again without a VM reset, and a module
  re-uploaded mid-session kept running the old copy. The `package` global
  is gone with it. A module that returns nothing now yields `nil` from
  `require()` rather than `true`. A module `require()`d from two places is
  loaded twice, so stateful modules (e.g. `data.min`, which registers the
  BLE receive callback) should be required once by the app and passed down.

### Fixed

- A break (Ctrl+C) is now raised exactly once. The break hook raised
  `interrupted` on every VM instruction until the running chunk had fully
  unwound, and `frame.sleep()` / `frame.standby()` raised a second
  `interrupted` of their own, so a `pcall` that caught the break was broken
  again on its next instruction and a script could not handle a break and
  clean up. The hook is now the only source of the error and raises it once;
  a `pcall` handler runs to completion. Restart (Ctrl+D), exit and
  light-sleep wake still unwind the chunk fully.
- Callbacks registered from Lua persist across a break. A break cleared
  `frame.bluetooth.receive_callback`, all `frame.button` callbacks, the
  `frame.imu` tap callback and the `frame.compression` and ANCS callbacks
  (and the ANCS subscription), so code that registers a callback once and
  relies on it across a break stopped receiving events. Only a VM restart
  (Ctrl+D) clears callbacks now.

## [0.8.11] - 2026-09-21

### Fixed

- Every Halo distributed the same Identity Resolving Key during pairing
  (the stack's identity IRK was a build-time constant, and the per-unit
  key handed to the pairing `info_req` was not the one that reached the
  peer). Windows indexes LE bonds by peer IRK and rejects a second device
  presenting one it already holds ("trying to distribute an Identity
  Resolving Key that is already used by a paired device", BTHUSB event
  35), so a Windows PC could bond with only one Halo at a time. The IRK is
  now derived per unit as `SHA-256("halo-irk-v1" || EUI-64)` and the same
  key is used for both the stack identity and pairing distribution.
  Existing bonds are unaffected: Halo advertises with its static address,
  so peers never consult the stored IRK.
- `frame.display.bitmap()` with a `width` of 0 divided by zero in native
  code (the width is the row stride used to derive the bitmap height) on
  the 2/4/16-colour indexed paths, raising a UsageFault that rebooted the
  device and dropped the BLE connection. `width` is now validated
  (1–32767) on every colour format, and out-of-range values raise a Lua
  error instead. Reported by Sigolon
- `require()` loaded staged `/lfs` modules through `luaL_loadbuffer()`,
  which accepts binary as well as text Lua chunks. Lua's binary loader
  doesn't validate bytecode operands, so a malformed binary chunk could
  corrupt VM state or crash the interpreter; a client is only ever
  expected to hand us Lua source, so the loader is now restricted to
  text chunks. Reported by Sigolon

## [0.8.10] - 2026-09-17

### Fixed

- `mem_manager` heap operations are serialised with a spinlock. `halo_malloc`
  / `halo_free` are called concurrently from the Lua REPL thread, Bluetooth
  host callbacks and the sfxr thread, and `sys_heap` is not thread-safe; the
  `mem_ctx.lock` mutex was initialised but never taken (#11, #12)
- `frame.imu.raw()` / `direction()` no longer fail with `-116` (QMC6308 data-ready
  timeout) after `frame.standby()`. The standby SUSPEND handler PM-suspends the
  magnetometer, but the always-on IMU service never receives RESUME and
  `imu_hardware_init()` short-circuited on `hardware_configured` before its
  PM-resume block; the PM-state reconciliation now runs on every entry (#13)
- `frame.standby()` no longer throws `"interrupted"` after a break signal that
  arrived while nothing was sleeping: the interrupt flag is now reset on entry,
  as `frame.sleep()` already did. A break during standby still interrupts it (#14)
- `frame.bluetooth.receive_callback` lost packets under load: data writes
  that arrived while a script was inside `frame.sleep()` (or any blocking
  call) overwrote each other so only the last one was delivered, and a
  burst during a busy Lua loop arrived as one concatenated blob. The BLE
  data path is now a framed queue drained on the Lua thread — one client
  write is one callback, in order, and a full queue refuses the write at
  the ATT level instead of dropping it (#15)
- `frame.compression.decompress()` delivered only the last block of a
  multi-block LZ4 frame to `process_function`; it now runs the callback
  once per block, in order (#15)
- `frame.microphone.aad_callback` ran its Lua function on the microphone
  driver's thread, concurrently with the REPL thread; it is now queued and
  delivered on the Lua thread like every other callback (#15)
- Ctrl+D (VM restart) sent while a script was busy waited for the script
  to finish; it now unwinds the script like Ctrl+C does (#15)

### Changed

- All asynchronous Lua callbacks (BLE data, button, IMU tap, mic AAD, ANCS)
  and the Ctrl+C / restart / standby breaks share one runtime-owned Lua
  hook, so two events landing together can no longer displace each other.
  The dedicated `lua_data` thread and its 8 KB stack are gone
  (`CONFIG_HALO_LUA_DATA_TASK_*` removed); `CONFIG_HALO_LUA_MAX_DATA_SIZE`
  now sizes the frame queue (#15)

## [0.8.9] - 2026-08-27

### Added

- `frame.speaker.start{gain=0..12}`: per-stream digital pre-gain into the
  protection limiter — lifts quiet sources (un-normalized TTS) toward the
  loudness ceiling, compresses hot ones; transient per stream (#6)
- `frame.speaker.start{budget=10..100}`: per-stream energy-budget
  override up to a firmware-clamped maximum — higher = louder ceiling at
  more battery current; any override engages the fast limiter attack
  that makes raised budgets safe (bench-validated at 100 against
  worst-case content); all protection stays active; transient per
  stream (#6)
- True-peak clamp on the protection chain's weighted drive
  (`MAX98357A_AUDIO_PROTECT_PEAK_CAP_PERCENT`, default 300 %): backstop
  against tall transient spikes inside the energy limiter's integration
  window (#6)
- Display-aware speaker energy budget: while the display is out of power
  save its current load shares the battery-protection IC with the
  speakers, so the audio budget is reduced (default 20 points,
  Kconfig-tunable) and restored when the display sleeps, ramped smoothly
  even mid-stream (#6)
- Runtime protection-chain tuning API for bench/listening builds
  (`MAX98357A_AUDIO_PROTECT_TUNING`, off in production), and a
  psychoacoustic bass enhancer stage (off by default) (#6)

### Changed

- Speaker voicing rebalanced for loudness: deeper cuts in the
  vibrotactile lows, +4 dB top band, protection HPF 130 → 200 Hz —
  blinded on-head A/B: louder speech at equal buzz (#6)
- Speaker protection sidechain weights recalibrated from bench current
  measurements: supply current tracks drive amplitude (not frequency),
  so the per-band weights are now uniform and the energy budget caps
  current honestly for any spectrum (#6)
- Speaker limiter releases upward when its budget rises mid-clamp
  (previously the gain ratcheted down until the content itself went
  quiet) (#6)
- Battery readings average a four-conversion ADC burst per fetch (#5)
- BLE Lua RX and audio-RX characteristics refuse ATT offset (long/prepared)
  writes (`NO_OFFSET`), matching the rest of the GATT database; one ATT write
  is treated as one complete message (#7)

### Fixed

- BLE Lua RX write handler no longer underflows the ring-buffer length on a
  zero-length or offset write, and continuation fragments no longer drop a
  byte or gain a stray newline. Originally reported by @cjfreeze in #4; this
  fix takes the fuller approach (#7)
- BLE Lua RX handler routes a bare data marker (`send_data("")`) as an empty
  frame instead of passing the marker byte to the Lua REPL (#8)
- Reported battery level converges toward the measured charge at a
  bounded rate (at most one percentage point per update after three
  confirming samples) instead of holding monotonic between charger
  events (#5)
- Battery level filter seeds from steady-state readings taken 15 s
  after boot instead of the first three fetches; until seeding
  completes the raw measured level is reported directly (#5)
- `frame.GIT_TAG` is populated in CI-built images: the build container's
  workspace is marked safe for git, so the build-time `git describe` no
  longer fails silently (#3)

### Documentation

- PROTOCOL.md errata: advertising and pairing summary, display and SDK
  corrections; button/pairing/BLE docs aligned with multi-bond
  behaviour; pairing flowchart vendored into the repo; SETUP.md
  container-build section hardened (#1)
- `frame.display.get_font_list()` return shape and example
  corrected (#2)

## [0.8.8] - 2026-08-17

### Added

- Native single/double/triple tap detection with a tuned, runtime-configurable
  detector: tap callbacks receive a kind argument, `frame.imu.tap_config()`
  exposes the tuning knobs (#273)
- Button hold ladder: 1 s app event / 2 s power-off / 5 s pairing / 15 s ship
  mode, with audio cues at each threshold (#278)
- Boot splash draws the device name at 16 px while unpaired (#279)
- Optional battery-state gating of the boot/shutdown cues, with a configurable
  voltage floor (default 3400 mV) (#285)
- `require()` caches modules in `package.loaded`, as standard Lua does (#260)
- Microphone `bit_depth = 8` produces real 8-bit samples via post-pipeline
  downconversion (#271)
- Device test battery: runner, README, and ported/repaired tests across
  display, file, power-management, and throughput (#247, #255, #261, #262,
  #263, #265, #280)

### Changed

- Display font replaced: FreeMono GFX fonts → Dogica 8 px pixel font
  (−12.5 KB flash); `set_font` sizes are now multiples of 8 (#246)
- Boot/shutdown cues play at the app-wide volume (#282)
- UART console clocked from SYST_PCLK instead of an unmanaged oscillator (#257)
- Public-repo preparation: public README / setup / flashing docs, license
  attributions, pre-patched fork pins, generic device names (#245, #276)

### Fixed

- Display: `canvas_set_pixel` R/B channel order, global palette stored as RGB
  (was YCbCr-quantised, tinting the defaults), `palette_offset` no longer
  wraps (#281)
- `require()` returns the module's own values instead of the filename (#260)
- `frame.time.zone()` applies the sign to minutes on negative offsets, and the
  setter returns the resulting zone (#259)
- Deep-sleep policy lock is held across light sleep (#266)
- BLE could remain silent after a deep-sleep wake due to stale `noinit`
  connection state (#278)
- Async sound worker no longer starved by busy Lua threads (#278)
- `get_se_revision()` no longer reads past the end of its buffer (#264)
- Canvas font ascent measured from `'H'` instead of a raw glyph index (#258)
- IMU: `heading()` documented as host-side; tap callback errors when the
  trigger is unarmed (#269)

### Removed

- `frame.on_wakeup()` — it only ever fired synchronously inside `standby()`;
  use sequential code after the call instead (#268)

### Documentation

- LC3 frame duration units clarified (µs/10, matching the Alif enum) (#242)
- Speaker bit-depth claim corrected; camera `quality` documented as real (#270)
- Skills/agent notes: logs skill requires the FS log backend (#277)

## [0.8.7] - 2026-07-20

### Added

- On-device acoustic echo cancellation for microphone + speaker duplex, with
  opt-in voice-band mode (~300–3400 Hz) (#240)

### Changed

- Battery SoC computed from a non-linear charge/discharge model with a
  charger offset, improving % accuracy (#243)
- Python device tests migrated from frameutils to brilliant-ble (#241)

### Fixed

- IMU `direction()` pitch/roll computed in the host frame (#244)

## [0.8.6] - 2026-07-14

### Added

- Pairing-aware boot splash and new startup/shutdown sounds (#228)
- iOS ANCS client + `frame.ancs` Lua API (#230)
- LE Audio microphone source (MICP/TMAP) + `frame.microphone.status()` (#231)
- 5-slot multi-bond with pairing-window semantics (#232)
- OTA/DFU lifecycle logging (#234)

### Fixed

- Reliable deep-sleep shutdown (#227); display power-save state synced across
  reconnect / VM restart (#229)
- BLE security audit: encryption-gated services, bond-delete DoS, heap/UAF
  fixes, ANCS + LE-Audio hardening (#235)
- Idempotent zephyr/alif patch application (#226)

## [0.8.5] - 2026-07-04

### Added

- Boot logo splash before the Lua runtime and an SFXR startup chime (#219,
  #220)
- `frame.sound` Lua API: play, play_async, stop, is_playing, SFXR presets
- MCUboot self-confirms the image at the end of a successful boot (#217)

### Changed

- Speaker protection: current-proxy energy budget replaces the stacked
  stream-side HPF/limiter (#222)
- LC3 mute logic for garbage/PLC frames, with hysteresis (#222)

### Removed

- Vestigial `modules/frame/` (#221)

## [0.8.4] - 2026-06-23

### Added

- Standby AAD voice wake re-armed on every standby sleep (#206)

### Changed

- Microphone DMIC DMA block interval 100 ms → 20 ms (#213)

Factory SE flashing package attached to this release; later releases reuse it.

## [0.8.3] - 2026-06-12

### Added

- Log memory (#204)

### Fixed

- AAD wake-up; `discard-duration-ms` disabled (#208)

## [0.8.2] - 2026-04-24

### Fixed

- AAD wake-up (#199)

## [0.8.1] - 2026-04-13

### Added

- CI setup (#178); wake from AAD (#186); standby wakes from AAD and IMU (#189)

### Fixed

- Unstable AAD wake from light sleep / standby (#193)

## [0.8.0] - 2026-04-04

### Added

- LED patterns (#173); factory-mode filesystem wipe (#177)

### Fixed

- Light-sleep sequencing (#179); display `lua_Integer` format specifier (#181)

### Removed

- Unused board directories (#174)
