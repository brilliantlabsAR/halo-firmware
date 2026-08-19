# Porting the dual-mic beamformer to firmware

Once an approach wins on real `/lfs` captures (expected: **BC**, the coherence
mask — see `README.md`), here is how it lands on-device. The whole change is
**mono-output and touches no BLE service definition**, so it avoids the
stale-GATT-cache OTA hazard that flipping `INPUT_STEREO` caused.

## The key architectural move: decouple *mic channels* from *source ASE channels*

The buzz saga proved a 2-channel LE-Audio *source*
is blocked (ROM + Android). Beamforming sidesteps that entirely: **capture two mics
internally, beam them down to one channel, ship one mono LC3 stream.** The phone
still sees exactly the mono source it sees today.

Concretely that means a build where:
- the **microphone front-end captures 2 channels** (the stereo PDM demux, already
  working — per-channel phase `ch2=0x1F/ch3=0x03` from the earlier fix), but
- **`LOCATION_SOURCE` stays a single bit** → 1 source ASE, 1 CIS, `channel_count=1`
  (i.e. the mono BLE config that ships today, unchanged).

Add a Kconfig, e.g. `CONFIG_HALO_BLE_AUDIO_INPUT_BEAMFORM`, that:
- forces the mic capture to 2 channels (independent of `INPUT_STEREO`, which stays
  `n` so the *source* is mono), and
- enables the beamform stage below.

## Integration point

`modules/halo/src/ble_audio.c`, the source encoder thread (`~line 615`). Today:

```c
ret = audio_microphone_read(&mic_buffer, &mic_buffer_len);   // interleaved int16, channel_count ch
...
for (frames) for (ch) { de-interleave ch -> channel_buffer; lc3_encode(channel_buffer); }
```

With beamforming, `mic_buffer` is 2-channel interleaved `[L0 R0 L1 R1 …]`. Insert a
reduction *before* the per-channel encode loop:

```c
/* mic_buffer: 2ch interleaved @ 16 kHz, 20 ms blocks (320 samples/ch).
 * Reduce to one mono channel with the beamformer, then encode as mono. */
beamform_process(&bf, (int16_t *)mic_buffer, mono_buf, samples_per_frame);
/* then the existing loop runs with channel_count == 1 over mono_buf */
```

`beamform_process()` consumes 2ch interleaved, emits 1ch — the C port of the winning
`dualmic.py` method. Everything downstream (SDU alloc, `audio_lc3_encode_frame`,
queue, CIS) is unchanged.

## What to port (BC = coherence mask) and the compute budget

Per 20 ms block (320 samples/ch @ 16 kHz), streaming STFT:
1. **Frame** 256-pt windowed (Hann), 128 hop (50% overlap) → ~24 ms algorithmic
   latency. Acceptable for voice; note it stacks on LC3's ~10 ms.
2. **2× real FFT** (L, R) — CMSIS-DSP `arm_rfft_fast_f32`, Helium-accelerated.
3. Per bin (129 bins): cross-spectrum `X_L·conj(X_R)`, `|X_L|,|X_R|`, IPD via
   `arm_atan2_f32` (or CORDIC), 1-pole smoothing of the auto/cross spectra.
4. Build the mask (B: IPD/ILD gaussians + voice band; C: MSC vs `sinc(2fd/c)`
   diffuse floor), multiply, floor, smooth.
5. Apply to `(X_L+X_R)/2`, **1× inverse FFT**, overlap-add.

Two 256 FFTs + one IFFT + ~129 bins of complex arithmetic every 10 ms hop is *tens
of µs* on the M55 with Helium — comfortably real-time. Do it in **f32** first
(simplest, and the FPU/MVE-F is there); consider Q15 only if cycles bite.

**Prereqs to confirm in the Zephyr build:** `CONFIG_CMSIS_DSP` (+ the transform /
statistics / fast-math submodules) and that MVE (`CONFIG_ARM_MVE*`) is on — I did not
confirm these are currently enabled. If CMSIS-DSP isn't wired, that's the first PR.

## Calibration on-device (Step 0)
- **Level match:** set per-channel programmable mic gain to null the measured L/R
  mismatch (the harness saw **−4.8 dB** on one `/lfs` pair). One-time, or a slow
  auto-trim on broadside-dominant frames.
- **Phase match:** a fixed fractional-delay/all-pass on one channel from a bench
  measurement. Method BC is fairly mismatch-tolerant (per-bin self-referencing), so
  this is second-order.
- **Spacing `d`:** measure once on the bench (endfire snap → cross-correlation lag)
  and bake in as a constant; only `f_c` and the diffuse-coherence curve use it, and
  the mask is not very sensitive to it.

## Why this is OTA-safe
The source stays one mono ASE, so ASCS ASE count and every GATT handle are identical
to today's shipping mono build. No service-definition change → **no stale-cache
"handle is invalid" OTA failure** (the class of bug that made `INPUT_STEREO` flips
painful). It's a pure DSP insertion behind a Kconfig; `n` = today's firmware exactly.

## Optional ceiling-raiser: Ethos-U55
Replace the hand-tuned BC mask with a small learned mask-estimator (few-layer
CRN/GRU on `[|X_L|,|X_R|,cos·IPD,sin·IPD,ILD,coherence]` per frame → mask), trained
on simulated two-mic mixtures using the *measured* `d` and mic responses (`synth.py`
is the seed of that simulator). int8 via Vela on the Ethos-U55. Bigger lift (data,
training, quantization, driver wiring — confirm Ethos is enabled in this build);
worth it only once BC on real captures sets the quality bar to beat.
