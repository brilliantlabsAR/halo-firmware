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

- `frame.speaker.start{gain=0..12}`: per-stream digital pre-gain into the
  protection limiter — lifts quiet sources (un-normalized TTS) toward the
  loudness ceiling, compresses hot ones; transient per stream
- `frame.speaker.start{budget=10..100}`: per-stream energy-budget
  override up to a firmware-clamped maximum — higher = louder ceiling at
  more battery current; any override engages the fast limiter attack
  that makes raised budgets safe (bench-validated at 100 against
  worst-case content); all protection stays active; transient per stream
- True-peak clamp on the protection chain's weighted drive
  (`MAX98357A_AUDIO_PROTECT_PEAK_CAP_PERCENT`, default 300 %): backstop
  against tall transient spikes inside the energy limiter's integration
  window
- Display-aware speaker energy budget: while the display is out of power
  save its current load shares the battery-protection IC with the
  speakers, so the audio budget is reduced (default 20 points,
  Kconfig-tunable) and restored when the display sleeps, ramped smoothly
  even mid-stream
- Runtime protection-chain tuning API for bench/listening builds
  (`MAX98357A_AUDIO_PROTECT_TUNING`, off in production), and a
  psychoacoustic bass enhancer stage (off by default)

### Changed

- Speaker voicing rebalanced for loudness: deeper cuts in the
  vibrotactile lows, +4 dB top band, protection HPF 130 → 200 Hz —
  blinded on-head A/B: louder speech at equal buzz
- Speaker protection sidechain weights recalibrated from bench current
  measurements: supply current tracks drive amplitude (not frequency),
  so the per-band weights are now uniform and the energy budget caps
  current honestly for any spectrum
- Speaker limiter releases upward when its budget rises mid-clamp
  (previously the gain ratcheted down until the content itself went
  quiet)

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
