# Dual-mic voice beamforming — offline prototyping harness

Goal: on the Halo's two edge-multiplexed T5838 PDM mics, **boost voice that arrives
at both mics at the same time** (the wearer, and whoever they face — both sit on the
perpendicular bisector of the mic pair, so zero inter-mic delay and equal level) and
**attenuate sound that arrives with a delay** (anything off to the side), restricted
to the human voice band. Output stays a single mono stream (what LE-Audio ships).

This directory is a **pure-offline** harness so we can design and A/B without
flashing. When real stereo `/lfs` captures land (`../record_mic_lfs.py`), the same
tools run on them unchanged. It also ships a **synthetic two-mic scene generator**
so every method can be validated *right now*, with no recordings, against a known
clean-target ground truth.

## Files
- `dualmic.py` — the DSP library (methods + calibration + metrics). Import it.
- `synth.py` — build a synthetic scene (broadside target + off-axis interferer +
  physically-correct diffuse field). CLI.
- `beamform.py` — run/score methods on a capture or synthetic scene. CLI.

Everything is `uv run` (PEP-723 inline deps: numpy + scipy). No install step.

## Quick start
```bash
cd applications/halo/tests/beamform

# self-validating demo — synthesise a scene and score every method (no capture)
uv run beamform.py --demo

# inspect a real capture's geometry / mic match
uv run beamform.py --calibrate ../lfs_pcm_16000hz_2ch_20260709_010015.wav

# process a capture and write <in>_BC.wav
uv run beamform.py ../lfs_pcm_16000hz_2ch_20260709_010015.wav --method BC --spacing-cm 10

# write listenable demo_*.wav for every method
uv run beamform.py --demo --write
```

## The geometry we exploit
Two omni mics at ±d/2. A plane wave from azimuth θ (0 = straight ahead) hits them
with inter-mic delay `τ = d·sinθ/c`. The target is at θ≈0 → **τ≈0: in-phase and
equal-level at every frequency.** Two trivial combinations anchor everything:

| | response vs angle | role |
|---|---|---|
| `SUM = L+R`  | `2·|cos(π f d sinθ/c)|` | broadside boosted; the linear beam |
| `DIFF = L−R` | `2·|sin(π f d sinθ/c)|` | **exact zero at broadside** → target-free noise reference |

Characteristic frequency `f_c = c/2d` (spatial-aliasing / diffuse-coherence null):

| d | max ITD @16 k | f_c |
|---|---|---|
| 3 cm | 1.4 samp | 5.7 kHz |
| 6 cm | 2.8 samp | 2.9 kHz |
| 10 cm | 4.7 samp | 1.7 kHz |
| 13 cm | 6.1 samp | 1.3 kHz |

**Honest limitation:** two mics on a left-right axis resolve *only* the left-right
axis. Anything on the vertical median plane — directly in front, behind, or above —
is broadside (zero ITD) and cannot be told apart. So this rejects sources to the
**sides** and cannot reject a source **directly behind** the wearer. For "talker in
front + interferers to the side" that is exactly the wanted behaviour.

## The three methods
- **A · `gsc_lite`** — SUM cleaned by an NLMS adaptive canceller driven by the
  target-free DIFF reference (a geometry-locked Generalized Sidelobe Canceller; the
  fixed broadside look direction makes the blocker exact, so there's no
  target-cancellation from steering error). Adapts a null onto a *dominant lateral
  interferer*.
- **B · `ipd_ild_mask`** — STFT soft mask: pass a T-F bin only if it looks broadside
  (inter-channel phase diff ≈ 0) **and** level-matched (ILD ≈ 0) **and** in the voice
  band. A *nonlinear* per-bin spatial filter — the direct encoding of "same arrival
  time." Frequency-dependent beamwidth via `accept_sin`; the IPD test is disabled
  above `f_c` (where phase wraps) and leans on ILD there.
- **C · `cdr_mask`** — STFT soft mask from the coherent-to-diffuse ratio: keep
  coherent (directional) energy, suppress diffuse (babble/wind/reverb) using the
  analytic diffuse coherence `sinc(2 f d/c)`. Direction-agnostic; pairs with B.
- **BC · `combined_mask`** (recommended default) — B × C: keep only **broadside AND
  coherent** voice.

> Correction to an earlier design note: a diffuse-noise *superdirective/MVDR*
> beamformer **collapses to plain delay-sum for a broadside target** on a symmetric
> pair (the steering vector `[1,1]` is an eigenvector of the diffuse coherence
> matrix), so it buys nothing here. The real third lever is the coherent/diffuse
> ratio (method C), not superdirectivity.

## What the validation shows (synthetic scenes, segmental SNR vs clean target)

| scene | SUM (baseline) | A | BC |
|---|---|---|---|
| dominant side talker (−6 dB, 70°, low diffuse) | ref | **+2.1 dB** | **+12.1 dB** |
| moderate side + diffuse (0 dB, 60°) | ref | −0.7 dB | **+10.3 dB** |
| diffuse-dominated babble (+3 dB, 60°) | ref | −0.6 dB | **+11.5 dB** |

**Headline finding (matches the small-array literature):** nonlinear T-F masking
(BC) beats linear/adaptive cancellation (A) by ~3–4× on a two-mic array. Why: the
optimal *linear* canceller's ceiling here is only ~**+3.8 dB** of target-fidelity
improvement (measured via an offline block-Wiener solve — the eye-catching "13 dB
SUM energy reduction" is mostly energy, not fidelity, because diffuse noise is
inter-channel-incoherent so DIFF can't predict it). Masking exploits the
**time-frequency sparsity of speech** to separate per-bin what no linear filter can.

**Recommendation:** ship **BC (mask)** as the primary enhancer. Method A is a modest,
optional add-on *only* when a recording shows a single dominant lateral interferer;
its NLMS already sits near the linear optimum, so don't bother upgrading it to RLS.

## Calibration first (Step 0)
All the phase math assumes the two mics are gain/phase matched. `--calibrate` on a
real capture already shows they may not be (one `/lfs` pair reads **−4.8 dB** L/R
level mismatch). Fix magnitude with the programmable mic gain; fix residual phase
with `calibrate_apply(delay_samples=…)`. Method B is the most mismatch-tolerant (it
self-references per bin); A is the least (mismatch leaks target into DIFF → the
double-talk `freeze_ratio` guard exists for exactly that).

**Measuring the spacing `d`:** global GCC-PHAT on voiced speech is unreliable (it
locks onto a pitch-period lag — see the `--calibrate` caveat). An **endfire transient**
(finger snap from hard off to one side) *should* give the cross-correlation lag =
`d/c`, but in practice a hand snap is reverberant and rarely truly endfire — ours read
1.9 samples (→"4 cm") when the arms are physically ~13 cm apart (that snap was only
~18° off-axis). **Trust the physical measurement.** Default `--spacing-cm` is now 13.

## Real-Halo findings (2026-07-09, `d`≈13 cm)
Measured on real `/lfs` captures (target = wearer's own voice broadside; interferer at
45°/90°; babble; PDM pop cropped 30 ms):
- **Mics are matched** (−0.3 dB after cropping the pop; the "−4.6 dB" seen on the raw
  clip was the pop skewing the RMS). No calibration needed.
- **The mask genuinely rejects off-axis energy ~9 dB**: bucketing every T-F bin by its
  inter-channel phase, BC gain is −6 dB at `|IPD|≈0` (broadside, kept) and floors at
  −15 dB for `|IPD|>1 rad` (off-axis, rejected).
- **Beware the scene-mean metric.** Aggregate level per scene barely moved (~1–2 dB)
  because the loud broadside own-voice dominates every clip (≈87% of the "interferer"
  clip's energy is at `|IPD|≈0`); the interferer was a quiet minority. The *per-IPD*
  breakdown is the trustworthy read, not the scene mean. To see the full benefit,
  re-record with the interferer at a level comparable to the target.
- **Low-frequency diffuse noise is a hard 2-mic limit**: below `f_c` a diffuse field is
  coherent with `|IPD|≈0`, so it looks broadside and can't be separated. Babble is only
  partly reduced (its off-axis / higher-freq part).
- The coherence (C) term also attenuates the *target* ~6 dB (own voice is near-field +
  reverberant → coherence <1); worth softening C's floor so it doesn't tax the target.

## Next steps
1. Capture real stereo `/lfs` scenes (target-only; target + side interferer;
   babble; wind) + one endfire snap for `d`.
2. Run `beamform.py --all` on them; listen to `demo_BC.wav` vs input.
3. Tune B/C knobs (`accept_sin`, `sigma_ild_db`, `floor_db`, `band`, `overest`).
4. Port the winner to firmware — see `FIRMWARE_PORT.md` (mono output → no GATT
   service change → no stale-cache OTA hazard).
