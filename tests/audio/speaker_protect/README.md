# speaker_protect host tests

Host-side (macOS/Linux) characterisation tests for the bone-conduction
speaker current-protection chain in
`drivers/audio/max98357a/speaker_protect.c`.

The DSP source has no Zephyr dependencies, so the **exact production code**
is compiled here with ASan/UBSan and verified against an independent
double-precision reference implementation of the sidechain.

```sh
make run
```

Covered:

- Small-signal frequency response (voice band fullness vs old stacked chain)
- Energy limiter caps the current-weighted envelope at the configured budget
  (mono, stereo in-phase worst case, LF content, full-scale square wave)
- Quiet content passes with unity gain (protection engages only near the
  trip region)
- Limiter ballistics: no per-cycle gain tracking (hold + slow release)
- Onset ramp on start and after reset
- Stereo channel state isolation
- Bit-exact state continuity across arbitrary buffer boundaries
- 8 kHz operation, parameter validation

These tests characterise the DSP only. The energy **budget**
(`CONFIG_MAX98357A_AUDIO_PROTECT_BUDGET_PERCENT`) must be calibrated on
hardware: play full-scale sine sweeps and real TTS at max volume (stereo),
measure battery current, and choose the largest budget that stays below the
battery protection IC trip point with margin.
