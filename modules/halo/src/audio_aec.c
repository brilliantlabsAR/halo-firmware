/*
 * Copyright (c) 2026 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * Acoustic echo cancellation: time-domain NLMS adaptive FIR that subtracts
 * the speaker's contribution from the microphone stream. See audio_aec.h
 * and applications/halo/tests/aec/README.md for the design background and
 * the offline prototype (aec.py) this mirrors.
 */

#include <halo/audio_aec.h>
#include <max98357a_audio.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <string.h>

#if defined(CONFIG_HALO_AUDIO_AEC_FDAF) && defined(CONFIG_CMSIS_DSP)
#include <arm_math.h>
#endif

LOG_MODULE_REGISTER(audio_aec, CONFIG_HALO_LOG_LEVEL);

#define AEC_SAMPLE_RATE 16000
#define AEC_TAPS        CONFIG_HALO_AUDIO_AEC_TAPS
/* largest mic block we expect: 20ms at 16kHz mono */
#define AEC_MAX_BLOCK   320
/* Window placement, measured (2026-07-13, chained pair captures on
 * Halo unit AND dev kit, three stimuli): the true physical echo delay is only
 * ~1-2ms (17-32 samples: structure-borne + PDM/I2S pipeline), NOT the
 * ~44ms the old consume-time-anchor era suggested. That is LESS than
 * the tap's 20ms commit granularity, so pairing a mic block with
 * already-committed reference is impossible without delay: the AEC
 * holds each mic block back one block (20ms mic latency) and processes
 * it when the next arrives, by which time the echo's reference has
 * been committed. Time accounting stays truthful for the ARRIVING
 * block (epochs unbiased, A/B-toggle-safe); only the PCM content is
 * swapped, so the window sits one block ahead of the held content and
 * the echo lands at d = 320 + 16*(physical) - AEC_REF_LEAD =
 * ~+145..160 taps - inside the window with margin on both sides.
 * (With the old
 * REF_LEAD of 352 and no hold-back the echo sat at d = -320..-335,
 * one block PAST the acausal edge - outside the window on every
 * device, which is why w converged to weight noise and ERLE <= 0.)
 * Overridable for host-side sweeps.
 */
#ifndef AEC_REF_LEAD
#define AEC_REF_LEAD    192
#endif
/* Margin servo. Device default ON (worn-validated on the Halo unit, 16f3d64:
 * unqualified win; the host harness cannot skew the two clocks so it stays
 * idle there). -DAEC_MARGIN_SERVO=0 to disable. The read window
 * is anchored on the mic capture clock while the write head advances on the
 * I2S emit clock; the two are independent Balletto clock branches ~50-250ppm
 * apart, so `margin = w - desired_end` drifts negative over a session and the
 * newest reference in each block gets silence-padded (partial cancellation ->
 * structured echo leaks -> server-VAD self-barge; the worn "63s reference
 * misalignment" cascade). The existing emit-side late-floor slides emit_epoch
 * but under-corrects a persistent skew (measured: +46ms/245s vs a 250ppm need).
 *
 * This is a continuous consumer-side servo that holds `margin` at the value it
 * had at the last re-anchor (`margin_anchor` - the geometry the frozen epochs
 * chose, where cancellation demonstrably works: +75..+187 on device, ~0 on the
 * host harness). When margin drifts a deadband below that, the servo reads the
 * reference a touch slower (advances the read pointer by less than a block) so
 * margin climbs back; when it runs a deadband above, it catches up. Holding
 * margin constant keeps the echo at a FIXED tap regardless of drift
 * (echo_tap = physical_delay*16 - margin, independent of both clocks), so the
 * echo never walks out of the causal window.
 *
 * Because the read pointer and `desired_end` are slipped by the SAME amount
 * (ref_skew_adj), `drift` is invariant under the servo - it cannot trip the
 * >160 resync, and it never disturbs a run with no skew (host: margin sits at
 * anchor, servo idle). Kept entirely on the mic thread (no cross-thread write
 * to emit_epoch); reset on every re-anchor. Tuned worn (host cannot skew the
 * two clocks). */
#ifndef AEC_MARGIN_SERVO
#define AEC_MARGIN_SERVO 1
#endif
#if AEC_MARGIN_SERVO
#ifndef AEC_MARGIN_SERVO_STEP
/* max samples/block of correction. Device default 4 (up from the built-in 2):
 * worn Halo unit vol-80 under barge-flush churn drove margin to -701 (min -885)
 * with the step-2 servo clawing back only ~2 samples/block - the reference
 * window slid off the write head (margin<0), firing the ref-unreliable deep
 * duck that scrambles NEAR-END too ("stop" stopped working). Doubling the step
 * halves recovery and keeps more collapses in the margin-positive (near-end-
 * friendly) regime. Still <=4 of 320 samples re-read/block, inaudible. */
#define AEC_MARGIN_SERVO_STEP 4
#endif
#ifndef AEC_MARGIN_DEADBAND
/* only correct once margin has drifted this many samples from the anchor, so
 * per-block window jitter (tap/mic-jitter, ms-quantized epochs) doesn't drive
 * the servo - only the persistent one-way clock skew does. */
#define AEC_MARGIN_DEADBAND 24
#endif
#ifndef AEC_MARGIN_RESYNC_FLOOR
/* positive floor for the margin captured at a resync re-anchor. When capture
 * time runs ahead of the emission write head, the epoch pairing puts the read
 * window AT or PAST the write head (margin <= 0), but the echo's reference is
 * always OLDER emission (behind the head) - reading the unwritten head serves
 * pads, and the servo then HOLDS that non-positive margin, arming the
 * ref-unreliable deep duck EVERY block. That duck ceilings the whole uplink
 * (near AND far end), so a single bad resync blanket-mutes the conversation
 * for the rest of the session and never releases (worn Halo unit session 222622:
 * a resync ~90s muted both ends to the end; cap_late floor only -5ms, under the
 * late-floor backstop threshold, so nothing re-anchored it). Pulling the window
 * back to a positive floor keeps the servo band and normal jitter non-negative
 * so the duck can't latch; the resync already wiped the filter, so the constant
 * read-position shift is re-learned for free. Sized just under the device's
 * healthy anchor range (+75..+187) and ~2.7x the deadband. */
#define AEC_MARGIN_RESYNC_FLOOR 64
#endif
#endif /* AEC_MARGIN_SERVO */
/* reference ring: 256ms of emission history. The consumer's window is
 * placed by capture time, and the mic driver queue can hold a mic block
 * for up to ~160ms (measured startup backlog) before the consumer sees
 * it - the ring must still hold the ref that echoes into that block:
 * 160ms backlog + 64ms window + jitter. (Paid for by shrinking
 * CONFIG_HALO_MEM_INTERNAL_SIZE.)
 */
#define REF_RING_SAMPLES 4096

/* NLMS parameters, mirroring tests/aec/aec.py (mu overridable for
 * host-side sweeps)
 */
#ifndef AEC_MU
#define AEC_MU            0.5f
#endif
#define AEC_DTD_THRESHOLD 2.0f
/* one-pole smoothing over ~50ms at 16kHz */
#define AEC_POW_ALPHA     (1.0f / (0.05f * AEC_SAMPLE_RATE))
/* NLMS regularization: floor on the normalization energy so a sparse or
 * near-silent reference window (playback onset, dithered idle) cannot turn
 * into huge adaptation steps; ~= TAPS x (-60dBFS rms)^2
 */
#define AEC_EPS           1e-3f
/* adaptation gate: the consumed reference block must exceed this RMS
 * (~-54dBFS) - below it there is no echo to learn, only junk to diverge on
 * (a paused speaker session keeps clocking dithered near-silence)
 */
#define AEC_REF_GATE_RMS  2e-3f
/* keep adapting through speech gaps up to this many blocks (~160ms) */
#define AEC_GATE_HANGOVER_BLOCKS 8
/* ramp mu back up over ~0.5s of adapted samples after each gate-on, so the
 * filter can't slam into a divergence burst at playback onset
 */
#define AEC_MU_RAMP_SAMPLES 8000
/* echo-estimate sanity: y^2 may not exceed this multiple of the mic's
 * smoothed power (|y| <= 3x mic RMS) - a bigger estimate is always wrong
 * and this bounds how loud any misadaptation can ever sound
 */
#define AEC_Y_CLAMP_P     9.0f
/* rumble immunity: the Halo mic carries a constant ~-26dBFS sub-100Hz
 * rumble - ~14dB LOUDER than the speech-band echo. Fed raw into NLMS it
 * dominates the gradient as an uncorrelated disturbance and the resulting
 * misadjustment noise wipes out the cancellation (measured ~0dB ERLE on
 * device). Fix: filtered-error LMS - the update uses a high-passed error
 * paired with an equally high-passed copy of the reference history (same
 * filter on both keeps the fixed point = the true echo path in-band),
 * while the echo estimate y still convolves the raw reference. The power
 * measures (DTD, mic-power clamp scale) use high-passed views too. The
 * audio output stays bit-faithful outside the subtraction.
 * Two cascaded one-poles at ~300Hz: the rumble sits ~14dB above the echo,
 * so a lower corner leaves enough bleed-through to dominate the update
 * (measured: residual = rumble leakage, not echo). Speech-band echo
 * (300-3400) is what duplex apps need cancelled; below the corner the
 * filter simply doesn't adapt or subtract.
 */
#define AEC_HPF_ALPHA     0.1111f /* 1 - exp(-2*pi*300/16000) */
/* band-limit the update from above as well: the mic's HF content (PDM
 * noise, ambient) carries almost no echo - measured on the dev kit the
 * full-band update domain has non-echo energy ~10dB ABOVE the echo
 * while the speech band has echo ~20dB above the floor, so a full-band
 * gradient is mostly weight noise and cancellation nets ~0 ERLE with a
 * verified-aligned window (w_norm2 converged ~6x the true path norm).
 * Three cascaded one-poles at ~3.8kHz on the same filtered views (error
 * and reference history alike keeps the in-band fixed point unbiased);
 * the subtraction itself stays full-band via the raw history. HF echo
 * (mostly distortion products anyway) is the residual suppressor's
 * job, not the linear FIR's. (Overridable; 1.0f = passthrough, for
 * host-side A/B of the band-limit itself.)
 */
#ifndef AEC_LPF_ALPHA
#define AEC_LPF_ALPHA     0.7757f /* 1 - exp(-2*pi*3800/16000) */
#endif
/* divergence guard: the dev kit's speaker-mic coupling measured only ~6dB down,
 * so a legitimate filter norm can reach a few - clamp well above that and
 * rescale BELOW the threshold so the guard doesn't pin the filter to it
 */
#define AEC_NORM_CLAMP    16.0f
/* leaky LMS: speech is pitch-periodic, so period-shifted tap sets are
 * indistinguishable to the gradient and NLMS spreads mass across ghost
 * copies (measured: ||w||^2 grew 250x the true path norm on a real
 * assistant-voice clip, ERLE ~0, while white noise converged at the
 * ceiling). A small per-block decay while adapting biases the solution
 * to minimum norm - ghosts (which the gradient does not restore) drain
 * away, the true path (which it does) stays. (Overridable for A/B.)
 */
#ifndef AEC_W_LEAK
#define AEC_W_LEAK        0.94f
#endif
/* Proportionate steps (IPNLMS-style): the physical echo path is
 * compact (~200 of 1024 taps), and on pitch-periodic speech the plain
 * gradient is ambiguous across period-shifted tap sets - uniform steps
 * grow ghost copies. Concentrating the per-tap step on already-strong
 * taps (with a uniform floor so new paths can still be acquired) keeps
 * the mass at the true IR. alpha blends uniform (0) vs fully
 * proportionate (1); gains are normalized to mean 1 and recomputed per
 * block. (Overridable for A/B; 0 = plain NLMS.)
 */
#ifndef AEC_PNLMS_ALPHA
#define AEC_PNLMS_ALPHA   0.75f
#endif
/* Pre-emphasis on the update views (ref and error alike): speech has a
 * ~-6dB/oct spectral tilt, and NLMS convergence on colored input is
 * limited by the eigenvalue spread - the strong low bands adapt, the
 * rest crawls (measured: real-voice ERLE stuck at +1.5dB against a
 * coherence-derived LTI bound of 6-10dB). A first-order pre-emphasis
 * flattens the tilt in the update domain; the fixed point is unchanged
 * (same filter on both views). DEFAULT OFF: on the synthetic harness it
 * lifts voiced ERLE 4->9dB, but on the real device it made voice WORSE
 * (-1.6 vs +1.5dB) - measured voice coherence FALLS with frequency
 * (0.89 at 300-800Hz to 0.39 at 1.6-3.4kHz), so whitening re-weights
 * the update toward the incoherent band. The correct exploitation of
 * the coherent 300-800Hz region is per-bin (subband/FDAF) adaptation,
 * not a global tilt. (Overridable for A/B.)
 */
#ifndef AEC_PREEMPH
#define AEC_PREEMPH       0.0f
#endif

/* Speaker-activity gate: cancellation - and its one-block mic hold-back -
 * only applies while the speaker is actually emitting. A mic-only session
 * (e.g. an AAD wakeup from standby) must add ZERO latency and zero DSP
 * load, so once the reference tap has been quiet for longer than the
 * filter's window could still be draining echo tail (64ms span + block
 * granularity), the mic path bypasses entirely. Engaging costs one block
 * of inserted silence (the hold-back priming, at playback onset where
 * there is no echo to cancel yet anyway); disengaging drops the stale
 * held block (by then >= a hangover old). The filter taps are kept warm
 * across idle so each playback resume adapts from the converged path.
 */
#define AEC_SPK_HANGOVER_MS 120

/* overload safety: if a 20ms block repeatedly takes longer than this to
 * process, the mic thread can't keep real-time - bypass rather than starve
 */
#define AEC_BLOCK_BUDGET_US 15000
#define AEC_OVERLOAD_BLOCKS 32

/* Epoch establishment window: the min-latency epoch estimators refine
 * only downward, and every post-convergence refinement moves the ref
 * window under the filter - a >160-sample one is a resync (history
 * wipe), a smaller one silently shifts the learned IR and forces a
 * refit (measured on the Halo unit as anchor churn and diffuse taps). The
 * PDM and I2S clocks are crystal-locked to ~20ppm (see the README
 * clock audit), so once established the epoch has nothing left to
 * legitimately track: any residual latency bias is constant and the
 * FIR taps absorb it. Refinements are applied only within this window
 * of the epoch being (re)established; later ones are counted but
 * suppressed (cap/emit "late" stats) so hardware can validate the
 * freeze. (Overridable for host-side A/B of the freeze itself.)
 */
#ifndef AEC_EPOCH_FREEZE_MS
#define AEC_EPOCH_FREEZE_MS 2000
#endif

/* Late-floor backstop for UNREPORTED capture losses. Every capture-epoch
 * observation is obs = now - mic_total/16; upstream sample loss the
 * consumer was never told about (e.g. a PDM FIFO overflow discarding
 * hardware samples) retards mic_total, so obs runs persistently LATE
 * relative to the frozen epoch and the reference window slips off the
 * ring at the loss rate - with the drift check blind to it, because both
 * of its operands are clocked by seen-samples. Genuine consumer backlog
 * always returns to its floor within a couple of seconds (the mic thread
 * catches up between stalls), so a windowed MINIMUM lateness that stays
 * above threshold is loss, not backlog: slide the epoch later by the
 * floor and let the drift check re-anchor (a counted resync) instead of
 * letting the window walk off the reference ring. The freeze window is
 * reopened on each slide so a too-far slide can refine back.
 *
 * The backstop is symmetric. epoch_observe only refines the epoch EARLIER
 * and only inside the freeze window, and the forward slide above only
 * moves it LATER, so once the freeze closes a too-LATE anchor has no way
 * home: a transient stall can over-slide it forward, then the flush that
 * ends the stall (mic_total jumps, obs drops back) lands after the freeze
 * recloses and is discarded as "late". Left there, cap_end_ms runs ahead
 * of real time, the reference window walks PAST the write head, margin
 * latches negative and the near-end crushes to silence (worn Halo unit,
 * session 212454: -90 dBFS for the last ~60s). So a windowed minimum that
 * stays persistently EARLIER than the epoch is the mirror signal - slide
 * the epoch earlier by the same under-corrected floor. Healthy runs hold
 * floor >= 0 (the epoch is the min), so the earlier-slide only ever undoes
 * an over-slide and cannot misfire on jitter.
 *
 * This is a BACKSTOP, deliberately slow to trip: sized losses (driver
 * drop and overflow ledgers) go through audio_aec_note_mic_loss and
 * never reach it, and under live duplex load the backlog floor is noisy
 * enough (11-28ms observations, 2026-07-13 the Halo unit run) that a tight
 * threshold over-slides and drives the margin negative (window too new,
 * tail pads eat the echo region).
 */
#ifndef AEC_LATE_FLOOR_WIN_MS
#define AEC_LATE_FLOOR_WIN_MS 2000
#endif
#ifndef AEC_LATE_FLOOR_MS
#define AEC_LATE_FLOOR_MS 12
#endif

/* Emit-side twin of the late-floor: when the I2S TX FIFO runs dry the
 * hardware stretches emission with no status raised (measured: ref tap
 * 15994.8 Hz vs kernel time over a 5-min duplex run - the same >500us
 * interrupt blackouts that overflow the PDM FIFO, hitting the I2S ISR),
 * while the tap credits a full block per completion. The write head
 * then falls behind real time and the margin walks negative at ~5
 * samples/s - off the ring in ~30 min. Producer observations are
 * ISR-stamped (no consumer-backlog noise), so a tight threshold works.
 */
#ifndef AEC_EMIT_FLOOR_MS
#define AEC_EMIT_FLOOR_MS 4
#endif

#if defined(CONFIG_HALO_AUDIO_AEC_FDAF)
/* Per-bin frequency-domain adaptation (partitioned-block overlap-save
 * FDAF). Motivation, measured on the Halo unit with a real assistant-voice
 * stimulus: echo coherence is strongly frequency-dependent (~0.89 at
 * 300-800Hz falling to ~0.39 by 1.6-3.4kHz), so a single full-band
 * NLMS step lets the strong coherent bins dictate adaptation while the
 * weak bins contribute weight noise - voice ERLE plateaued at +2.4dB
 * against a ~10dB low-band coherence bound. Per-bin normalization
 * adapts each bin at its own SNR: the coherent low band converges deep,
 * the incoherent HF simply doesn't move (its step is masked off).
 *
 * Geometry: FFT frames of FD_N=1024 over the raw reference history,
 * hopping one 20ms mic block (FD_H=320); FD_K partitions of 320 taps
 * each, partition k using the spectrum from k hops ago. Output block =
 * last FD_H samples of IFFT(sum_k W_k . X_{m-k}) - with 320-tap
 * partitions and a 1024-point FFT the last 705 circular-convolution
 * outputs are linear, so the block is exact. The gradient
 * conj(X_{m-k}).E (E = FFT of the error block zero-padded on the left)
 * is linear in its first FD_N-FD_H lags; the constraint projection
 * (IFFT, keep the first 320 taps, FFT) therefore removes all circular
 * aliasing. It runs round-robin, one partition per block, and doubles
 * as the export of time-domain taps into aec.w (same orientation as
 * the NLMS build: w[AEC_TAPS-1-delay]) for aec('dump') diagnostics.
 *
 * Everything around the filter core - reference ring, stream epochs,
 * hold-back, speaker-activity bypass, ref gate, DTD, mu ramp, y clamp,
 * paired capture - is shared with the time-domain build.
 */
#define FD_N    1024
#define FD_H    AEC_MAX_BLOCK
#define FD_K    CONFIG_HALO_AUDIO_AEC_FDAF_PARTS
#define FD_BINS (FD_N / 2)
/* adaptation band: ~312Hz-3.8kHz (bin width 15.625Hz), the same band
 * the time-domain build's filtered-error update covered. Below it lives
 * the mic rumble, above it the echo is incoherent (distortion products)
 * - those bins never adapt, so the filter neither chases rumble nor
 * accumulates HF weight noise, and (unlike the time-domain build) it
 * subtracts nothing outside the band it can actually estimate.
 */
#define FD_BIN_LO 20
#define FD_BIN_HI 243
/* Low-band echo cap: the blanket suppressor ceiling extends DOWN to here
 * (~203Hz), below the adaptation band [FD_BIN_LO, FD_BIN_HI]. The loud
 * bone-conduction echo peaks at 220-312Hz - below the adaptation floor, so it is
 * never cancelled - and is speech-shaped enough to trip the server VAD (the
 * residual self-interruption a downstream band-pass could not remove without
 * cutting near-end voice). Capping these bins is prediction-INDEPENDENT (no
 * per-bin Wiener gain here, just the blanket g_cap), so it cannot destabilize
 * the NLMS the way band-limiting the ADAPTATION this low would. The cap still
 * lifts with the in-band gate on near-end/idle. */
#ifndef FD_CAP_BIN_LO
#define FD_CAP_BIN_LO 13   /* ~203Hz */
#endif
/* hops until the reference history frames are clean after a wipe: the
 * FFT frames span FD_N + (K-1)*H samples, and while they still contain
 * the wipe's hard zero edge, its rectangular-gating leakage spreads
 * reference energy across the whole band and poisons the per-bin
 * gradient - adaptation on such frames CORRUPTS W (measured on the
 * host harness: hot-mu onset ERLE -3.3dB vs +1.3 old-ramp, and warm
 * re-engages losing ~5dB re-learning what edge-gated frames unlearned).
 * Prediction and subtraction stay on - the pre-wipe silence is real.
 */
#define FD_HIST_FILL_HOPS ((FD_N + (FD_K - 1) * FD_H + FD_H - 1) / FD_H)
/* per-block one-pole for the spectral powers; ~= the time-domain 50ms */
#define AEC_FD_POW_ALPHA 0.33f
/* per-bin leak while adapting: the per-bin normalized step still walks
 * on noise in bins where the echo is weak; a light leak drains what the
 * gradient doesn't restore. Much lighter than the time-domain build's
 * 0.94 - per-bin normalization removes the pitch-ghost ambiguity that
 * leak was fighting there. (Overridable for host A/B.)
 */
#ifndef AEC_FD_LEAK
#define AEC_FD_LEAK 0.998f
#endif
/* per-bin NLMS regularization, relative to the mean in-band ref power */
#define AEC_FD_EPS_REL 1e-3f
/* per-bin step size: swept on the host harness - 0.25 beats the
 * time-domain build's 0.5 on voice (10.9 vs 9.6dB) and HF-noise (14.3
 * vs 12.1dB) scenarios at a negligible cost in noise-convergence speed;
 * 1.0 is worse everywhere. (Overridable for A/B.)
 */
#ifndef AEC_FD_MU
#define AEC_FD_MU 0.25f
#endif
/* Cold-onset schedule: a freshly-wiped filter (only enable() wipes W;
 * resyncs and speaker-idle gaps keep it warm) starts adapting at
 * MU + MU_HOT_EXCESS and the excess decays by MU_SETTLE per adapted
 * block (= exp(-320/8000): ~0.5s time constant), replacing the old
 * 0.5s linear soft-start. The soft-start guarded against onset
 * misalignment, which the physical-time anchoring has since fixed; the
 * per-bin max(P,inst) normalization bounds every hot step. Measured
 * offline on real voice captures (offline_perbin.py): onset-1s ERLE
 * with the suppressor goes +3.5 -> +17.5dB with the hot schedule,
 * because the suppressor is only as good as the prediction it gates
 * on. A WARM re-engage skips all of this by construction: the excess
 * has already decayed. (Overridable for A/B; 0 = plain MU.)
 */
#ifndef AEC_FD_MU_HOT_EXCESS
#define AEC_FD_MU_HOT_EXCESS 0.75f
#endif
#define AEC_FD_MU_SETTLE 0.960789f

/* Residual suppressor (the barge-in onset stage): per-bin over-
 * subtraction on the FDAF's output, gated by the ratio of predicted-
 * echo to residual power - it needs no convergence of its own, so it
 * covers the linear filter's adaptation window (the "first ~3s of a
 * reply pass uncancelled" gap) and the nonlinear floor the FIR can
 * never reach. Gain rule per bin (smoothed powers, one-pole attack/
 * release): g = max(1 - BETA * Sy/Se, FLOOR). The POWER-domain ratio
 * (not amplitude) is deliberate: measured offline it costs ~2dB of
 * echo suppression but halves the double-talk damage to the wearer's
 * voice (-2.2dB median vs -3.9 at amplitude, same BETA), because a
 * bin where near-end dominates has Sy/Se << sqrt(Sy/Se). Gains are
 * applied in the existing 1024-pt overlap-save framework through a
 * zero-phase kernel rotated to causal support [0, 2D] and Hann-
 * windowed: the spectral product is then alias-free (kernel + block
 * fit the FFT) at the cost of a constant D-sample (8ms) group delay,
 * which exists only while playback is active. Offline on real voice
 * captures: onset-1s ERLE +0.7..+1.5dB -> +9.3..+17.5dB.
 * (BETA/FLOOR overridable for host A/B; BETA=0 = passthrough gains.)
 */
#ifndef AEC_SUP_BETA
#define AEC_SUP_BETA  1.5f
#endif
#ifndef AEC_SUP_FLOOR
#define AEC_SUP_FLOOR 0.1f
#endif
/* Onset boost (the head-worn barge-in fix, 2026-07-13): each reply's onset
 * leaks enough echo to trip the server VAD before the steady suppressor
 * re-establishes - measured head-worn as a false barge-in on EVERY reply
 * onset while a 29s converged stretch stays Silero-silent (see
 * tests/aec/README.md). For AEC_SUP_ONSET_HOPS hops after each onset the
 * suppressor runs at ONSET_BETA / ONSET_FLOOR and eases linearly back to
 * the steady BETA / FLOOR - it needs no convergence of its own, so it
 * crushes the onset residual immediately. Armed on the REFERENCE GATE's
 * rising edge (silence->active), which is the true per-reply onset: the
 * round-8 driver silence-feed keeps the speaker session engaged across
 * reply gaps, so a speaker-idle re-engage fires only once per session.
 * (Overridable for host A/B; ONSET_HOPS=0 disables the boost.)
 */
/* The onset window must bridge the whole linear-filter convergence, not
 * just the first hop: head-worn (2026-07-13) a 1s window pushed the false
 * barge-in from ~1s to ~2s of playback but every reply still died at ~2s -
 * the duck faded before the FIR converged, so the echo re-emerged. The
 * window now HOLDS the full duck for ONSET_HOLD_HOPS, then eases to steady
 * over the remaining (ONSET_HOPS - ONSET_HOLD_HOPS) hops. The wearer's
 * uplink is attenuated for the hold, so this is a firmware barge-in grace
 * window; it lets the first reply survive to convergence, after which the
 * kept-warm filter carries subsequent replies. */
/* Device defaults 45 / 20 (~0.9s total / ~0.4s full-strength), down from
 * 175 / 125 (~3.5s / ~2.5s). Worn-tuned on the Halo unit: the long hold was sized for
 * the cold-start first reply, but the voice-mode band-pass (mic_voice_bandpass)
 * now confines near-end to ~300-3400Hz - exactly the band the onset cap crushes
 * - so a genuine barge in a reply's first ~2.5s was flattened. Warm restarts
 * reconverge in a few hundred ms, and the low-band cap (FD_CAP_BIN_LO) +
 * band-pass carry the onset echo, so a short hold lets barge-ins land fast while
 * keeping the onset scrambled below the server VAD. -D... to override. */
#ifndef AEC_SUP_ONSET_HOPS
#define AEC_SUP_ONSET_HOPS 45   /* ~0.9s at 20ms/hop (hold + ease) */
#endif
#ifndef AEC_SUP_ONSET_HOLD_HOPS
#define AEC_SUP_ONSET_HOLD_HOPS 20 /* ~0.4s full-strength before easing */
#endif
#ifndef AEC_SUP_ONSET_BETA
#define AEC_SUP_ONSET_BETA 4.0f  /* eased to AEC_SUP_BETA over the window */
#endif
#ifndef AEC_SUP_ONSET_FLOOR
#define AEC_SUP_ONSET_FLOOR 0.02f /* -34dB max at onset, eased to AEC_SUP_FLOOR */
#endif
/* Onset blanket ceiling: the beta/floor boost only deepens suppression the
 * predicted-echo/residual RATIO already asks for, so it does nothing when
 * the linear filter under-predicts the onset echo (the head-worn case - the
 * mic echo audibly quiets but has no suppressor "alien" texture, i.e. the
 * ratio gains barely move). This is a hard, prediction-INDEPENDENT ceiling
 * on every in-band gain, eased from ONSET_GCAP up to 1.0 over the window: a
 * spectrally-flat duck (applied through the same alias-free kernel, so no
 * musical noise) that guarantees the uplink is >= -20dB during the onset
 * regardless of what the filter predicts - it also ducks a wearer barge-in
 * for that ~1s (the standard grace-window trade). ONSET_GCAP=1.0 disables it.
 */
#ifndef AEC_SUP_ONSET_GCAP
#define AEC_SUP_ONSET_GCAP 0.02f /* -34dB uplink ceiling at onset -> 1.0 (off).
				  * Head-worn (2026-07-13), -20dB left the onset
				  * voice partly audible and a couple of reply
				  * starts still self-interrupted; -34dB mutes
				  * the onset below the server VAD. */
#endif
/* Steady blanket ceiling the onset g_cap eases DOWN to (experiment B): the
 * onset window scrambles the echo's speech structure with the prediction-blind
 * blanket cap, then lifts it to this value. 1.0 = current behaviour (cap fully
 * off after onset, echo residual regains speech structure and can re-trip the
 * VAD mid-reply). <1.0 keeps a sustained spectrally-flat ceiling on every
 * in-band bin so the structure stays scrambled - but it is prediction-blind,
 * so it also caps the wearer's near-end voice by the same amount (option A
 * gates this on the double-talk detector to spare near-end). */
#ifndef AEC_SUP_STEADY_GCAP
#define AEC_SUP_STEADY_GCAP 0.25f /* device default (worn-validated): sustain a
				   * -12dB spectrally-flat ceiling past onset so
				   * the mid-reply echo structure stays scrambled
				   * below the server VAD. Prediction-blind, so it
				   * would also cap near-end - the envelope gate
				   * (AEC_SUP_GCAP_ENV_GATE) releases it on genuine
				   * near-end voice. 1.0 = cap lifts after onset
				   * (echo residual regains speech structure and
				   * can re-trip the VAD mid-reply). */
#endif
/* Option A: gate the sustained ceiling on the double-talk detector. The
 * blanket cap is prediction-blind, so with B alone it also caps the wearer's
 * barge-in. When the DTD sees near-end (p_err >= AEC_DTD_THRESHOLD * p_ref)
 * this lifts the sustained cap back to 1.0 for that block, so the echo stays
 * scrambled whenever the wearer is silent but their voice is never capped.
 * (The onset window's deep cap still holds regardless - the grace trade.)
 * 0 = B (cap always sustained); 1 = A. No effect when STEADY_GCAP==1.0. */
#ifndef AEC_SUP_GCAP_DTD_GATE
#define AEC_SUP_GCAP_DTD_GATE 0   /* option A (power-DTD release). Noise-fragile
				   * - fires on any energy, so it bleeds echo in
				   * noise. Superseded by the envelope gate;
				   * kept default-off as a documented dead-end. */
#endif
/* Release ratio for A's gate: lift the sustained cap when p_err >= RATIO*p_ref.
 * The adaptation-freeze DTD (AEC_DTD_THRESHOLD=2.0) only trips on loud near-end;
 * the cap must release for quieter barge-in too, so this is more sensitive. */
#ifndef AEC_SUP_GCAP_DTD_RATIO
#define AEC_SUP_GCAP_DTD_RATIO 0.5f
#endif
/* Envelope release gate (supersedes option A): lift the sustained ceiling only
 * for genuine near-end VOICE, using an echo-removed excess-residual onset
 * detector that stationary room noise cannot trip (option A's exact failure).
 *
 * The linear filter under-predicts the voice echo (Sy is tiny vs the residual),
 * so the echo cannot be removed from the residual using the prediction Y.
 * Instead remove it with the STRONG reference envelope: the echo residual is
 * ~KAPPA*sqrt(p_ref). excess = max(0, sqrt(p_err) - KAPPA*sqrt(p_ref)) is then
 * ~0 during echo-only, = near-end during a barge-in, and = the (stationary)
 * noise floor in a noisy room. A slow-floor onset detector (floor tracks the
 * ambient level with a long time constant, NO freeze - freezing locks on to
 * loud noise) fires when a near-end burst rises above the low echo-only floor
 * but stays quiet on stationary noise (fast ~ floor -> ratio test fails) and on
 * echo (excess ~ 0). Validated offline on the 3 real cases (case_echo/noise/
 * nearend: echo 0%, near-end fires >90%, loud-noise 2%) using per-block
 * p_err/p_ref from the actual firmware (host/aec_wav -DAEC_WAV_DUMP).
 *
 * Priority order: (1) noise must NOT self-interrupt -> bias to capped;
 * (2) barge-in may be harder in noise (OK); (3) quiet-room barge-in must work.
 * Device default ON (worn-validated on the Halo unit, 2026-07-14: breaks the reply
 * self-interruption loop, echo crushed to gmean 0.03-0.20 during replies).
 * Requires STEADY_GCAP < 1 to have anything to gate, and the margin servo under
 * it (a collapsed reference reads p_ref~0 and defeats the gate's PREF_MIN
 * scoping). See tests/aec/README.md gate-release chapters.
 * -DAEC_SUP_GCAP_ENV_GATE=0 to disable. */
#ifndef AEC_SUP_GCAP_ENV_GATE
#define AEC_SUP_GCAP_ENV_GATE 1
#endif
/* Echo-removal coupling (excess = max(0, sqrt(p_err) - KAPPA*sqrt(p_ref))).
 * Roughly volume-independent (echo and reference both scale with volume);
 * robust across ~0.09-0.3 offline. */
#ifndef AEC_SUP_GATE_KAPPA
#define AEC_SUP_GATE_KAPPA 0.15f
#endif
/* Fast-envelope smoothing of the excess (~0.5 = 2-block attack). */
#ifndef AEC_SUP_GATE_FAST_A
#define AEC_SUP_GATE_FAST_A 0.5f
#endif
/* Slow ambient-floor smoothing (~0.01 = ~2s). NO freeze. */
#ifndef AEC_SUP_GATE_FLOOR_A
#define AEC_SUP_GATE_FLOOR_A 0.01f
#endif
/* Release when fast > RATIO*floor AND fast-floor > ABSFLOOR (sqrt(p_err) units;
 * ABSFLOOR just blocks firing on tiny excess in near-silence). */
#ifndef AEC_SUP_GATE_RATIO
#define AEC_SUP_GATE_RATIO 2.0f
#endif
#ifndef AEC_SUP_GATE_ABSFLOOR
#define AEC_SUP_GATE_ABSFLOOR 0.5f
#endif
/* Release hangover (blocks) to hold through brief near-end dips (~1s). */
#ifndef AEC_SUP_GATE_HANG
#define AEC_SUP_GATE_HANG 50
#endif
/* Rising-edge (transient) release path (worn latency lever, 2026-07-14).
 *
 * The level test above (fast > RATIO*floor) is echo-latency-bound: the slow
 * floor tracks up to the steady echo-excess level F during a reply, so a
 * barge-in only fires once fast exceeds ~RATIO*F - i.e. the near-end voice must
 * out-power the echo residual. That took ~2s worn (short "stop" never survived).
 *
 * The edge path is orthogonal and F-independent: a genuine voice ONSET makes the
 * fast EMA jump above its own ~MID_A baseline within 1-2 hops, while steady echo
 * and stationary noise sit at fast~=mid (no edge). Because ex already subtracts
 * KAPPA*sqrt(p_ref), an echo syllable's own envelope is largely cancelled, so the
 * edge is near-end-selective. Fires the release in ~1-3 hops (~20-60ms); the
 * hangover then holds it. Set EDGE_ABS very high to disable the edge path and
 * fall back to the pure level test.
 *
 * mid: medium EMA of the excess, the short-term baseline the edge rises above.
 * ~0.08 => ~12-hop (~240ms) time constant: fast enough to reset between the
 * assistant's syllables (stays edge-selective), slow enough that a voice onset
 * clears it before it catches up. */
#ifndef AEC_SUP_GATE_MID_A
#define AEC_SUP_GATE_MID_A 0.08f
#endif
/* Near-silence guard: (fast - mid) must exceed this ABSOLUTE increment
 * (sqrt(p_err) units) before an edge can fire, so the mid~=0 idle case can't
 * trip on a tiny excess. Offline (case_noise/nearend) the excess magnitudes are
 * large, so this is inert vs real events; EDGE_RATIO does the discrimination. */
#ifndef AEC_SUP_GATE_EDGE_ABS
#define AEC_SUP_GATE_EDGE_ABS 0.6f
#endif
/* AND fast > EDGE_RATIO*mid - the noise-robustness dial. A voice onset spikes
 * fast well above its ~240ms mid baseline; stationary noise fluctuates within a
 * smaller ratio. Offline oracle (case_noise): RATIO 1.3 -> 11.7% false release
 * (5 multi-block trips), RATIO 1.8 -> 0 raw trips (== the validated level-only
 * baseline) while case_nearend still fires 9x across the burst. So 1.8 is the
 * safe default; lower it worn (watch the false-barge count) if the release is
 * still too slow on a real barge-in. NOTE this reintroduces a mild coupling to
 * the echo level via mid, but mid's 240ms tau (vs the level floor's ~2s) dips in
 * the gaps between the assistant's syllables, so a barge-in lands sooner. */
#ifndef AEC_SUP_GATE_EDGE_RATIO
#define AEC_SUP_GATE_EDGE_RATIO 1.4f
#endif
/* Sustain-scoping: only apply the sustained ceiling when the reference shows
 * real echo (p_ref above this). Idle/dither has ~0 p_ref so the cap lifts and
 * cannot inject (host scenario 3). Well above dither, well below any real echo. */
#ifndef AEC_SUP_GATE_PREF_MIN
#define AEC_SUP_GATE_PREF_MIN 0.05f
#endif
/* Live reference-collapse hold (companion to PREF_MIN scoping). PREF_MIN reads a
 * ~0 read-window p_ref as "idle" and lifts the cap. But a barge-in that flushes
 * the speaker queue starves the reference tap: the aligned window momentarily
 * serves silence (p_ref -> ~0) WHILE real reply audio is still being written to
 * the ring and its echo is at the mic - and margin stays POSITIVE, so the
 * REF_UNREL (margin < 0) fail-safe misses it. Left alone the cap lifts and the
 * new reply's echo self-interrupts for ~20s until the read window re-anchors.
 * Fix: while REAL (non-silent) playback was fed within this many ms, do NOT let
 * a p_ref collapse lift the cap - hold the echo-scramble ceiling. Must exceed
 * intra-reply word gaps yet stay well under inter-reply idle gaps so genuine
 * idle still lifts (no dither injection). Live-only; offline (perfectly aligned
 * reference) never reproduces the collapse. */
#ifndef AEC_SUP_PLAYBACK_HOLD_MS
#define AEC_SUP_PLAYBACK_HOLD_MS 400
#endif
/* Reference-unreliable fail-safe duck. When the reference window slides off the
 * WRITTEN part of the ring (margin < 0 - emission fell behind the mic timeline
 * under BLE-duplex starvation; the drift-resync check is mic-clocked on both
 * sides and blind to it, so no resync fires), the consumer serves pads:
 * p_ref -> ~0 and the linear filter cannot cancel, so the residual is the FULL
 * uncancelled (structured) echo. In that state (a) the PREF_MIN sustain-scoping
 * would falsely read the ~0 p_ref as idle and LIFT the cap, and (b) the gate's
 * echo removal (excess = sqrt(p_err) - KAPPA*sqrt(p_ref)) breaks because
 * p_ref~0, so the echo looks like near-end. Both let the structured echo re-trip
 * the server VAD (the worn 63s cascade). Fail SAFE: hold the ceiling on and
 * re-arm the deep onset duck so the uncancelled echo stays scrambled below the
 * VAD until the reference recovers. Genuine barge-in is ducked for the collapse
 * (acceptable - priority 2). Hangover (blocks) bridges brief margin dips. */
#ifndef AEC_SUP_REF_UNREL_HOPS
#define AEC_SUP_REF_UNREL_HOPS 25
#endif
/* Resync re-arm (the evaporation-burst fix, 2026-07-13): a reference resync
 * (drift re-anchor from PDM-overflow churn, ~0.4/s head-worn) WIPES the
 * reference history, so the suppressor's echo prediction Y - and thus Sy -
 * collapses for the ~FD_HIST_FILL_HOPS it takes the FFT frames to refill.
 * The ratio gains 1 - BETA*Sy/Se snap to 1 and the echo passes uncancelled
 * for that burst: a false barge-in slips through, and the server then answers
 * its own echo into a phantom-VAD cascade (README "evaporation burst").
 * A resync is the same "prediction not established" condition as a reply
 * onset, so re-arm the onset duck to cover it - critically its
 * prediction-INDEPENDENT blanket ceiling (g_cap), which ducks regardless of
 * the collapsed Sy. Far SHORTER than the full onset window (~1s vs ~3.5s):
 * only the refill gap needs covering, and resyncs are frequent enough that a
 * full-length re-arm would keep the uplink permanently ducked and kill all
 * barge-in. The default equals the onset ease span, so the duck starts at
 * full ONSET_GCAP strength (ob=1.0) and eases linearly to steady over the
 * window with no flat hold. Armed with MAX (see the resync branch) so a
 * resync mid-reply never shortens the reply's own, longer onset duck.
 * (Overridable for host A/B; RESYNC_HOPS=0 disables the re-arm.)
 */
#ifndef AEC_SUP_RESYNC_HOPS
#define AEC_SUP_RESYNC_HOPS 50   /* ~1s ease from ONSET_GCAP back to steady */
#endif
#define AEC_SUP_D     128
#define AEC_SUP_KLEN  (2 * AEC_SUP_D + 1)
#define AEC_SUP_POW_A 0.33f
#define AEC_SUP_ATT   0.7f
#define AEC_SUP_REL   0.3f
#define AEC_SUP_NBINS (FD_BIN_HI - FD_BIN_LO + 1)
/* cos(2*pi / (KLEN - 1)), for the Hann-window cosine recurrence */
#define AEC_SUP_HANN_C 0.99969881f

#if (CONFIG_HALO_AUDIO_AEC_FDAF_PARTS * 320) > CONFIG_HALO_AUDIO_AEC_TAPS
#error "AEC_TAPS must cover FDAF_PARTS * 320 taps (shadow tap export)"
#endif
#if CONFIG_HALO_AUDIO_AEC_TAPS < 704
#error "AEC_TAPS + one block must cover the FDAF's 1024-sample FFT frame"
#endif
#endif /* CONFIG_HALO_AUDIO_AEC_FDAF */

static struct {
	/* reference FIFO: single ISR producer, single thread consumer */
	int16_t ref_ring[REF_RING_SAMPLES];
	atomic_t ref_widx;
	atomic_t ref_ridx;

	/* filter state, owned by the mic consumer thread. In the FDAF
	 * build w holds the constrained time-domain taps exported for
	 * diagnostics (aec('dump')), refreshed round-robin.
	 */
	float w[AEC_TAPS];
	/* reference history, oldest first; x[AEC_TAPS-1 + n] aligns with
	 * output sample n of the current block
	 */
	float x[AEC_TAPS + AEC_MAX_BLOCK];
	/* band-passed copy of the reference history: the filtered-error
	 * update pairs the band-passed error with this (same filter on
	 * both keeps the fixed point = the true echo path in-band), while
	 * the echo estimate y convolves the raw history above. The FDAF
	 * build needs it too: its update views must be STREAMING-filtered
	 * before the FFT gating - the mic rumble sits 14dB above the echo,
	 * and gating it with the frame's rectangular window leaks it
	 * across the whole adaptation band (rect sidelobes fall ~6dB/oct),
	 * which corrupts the per-bin gradient exactly like the full-band
	 * one (measured: in-band ERLE 11.8 -> 2.5dB with the rumble on).
	 * A per-bin mask cannot remove what leakage has already spread.
	 */
	float xf[AEC_TAPS + AEC_MAX_BLOCK];
#if !defined(CONFIG_HALO_AUDIO_AEC_FDAF)
	/* proportionate step gains (IPNLMS-style), recomputed per block */
	float wgain[AEC_TAPS];
	float xf_norm2;
#endif
	float p_ref;
	float p_err;
	float p_mic;
	/* LF trackers for the high-passed views (mic, ref, error) */
	float mic_lf1;
	float mic_lf2;
	float ref_lf1;
	float ref_lf2;
	float err_lf1;
	float err_lf2;
	/* LP states band-limiting the update views from above (ref, error) */
	float ref_lp[3];
	float err_lp[3];
	/* pre-emphasis memories for the update views (ref, error) */
	float ref_pe;
	float err_pe;

	/* adaptation gate / onset ramp (mic consumer thread) */
	uint32_t gate_hangover;
	uint32_t mu_ramp;
	/* cold-onset hot-mu excess, decaying per adapted block (FDAF) */
	float mu_hot_excess;
	/* hops since the ref history was wiped (FDAF adapt guard) */
	uint32_t fd_hist_fill;

	atomic_t enabled;
	bool synced;
	bool ref_format_ok;
	bool warned_mic_format;
	bool warned_ref_format;
	uint32_t resyncs;
	uint32_t norm_clamps;
	uint32_t ref_underruns; /* consume events that had to pad */
	uint64_t ref_pads;      /* total padded samples while synced */
	/* window-placement observability: margin = ring write index minus
	 * window end at each consume. Negative = the window's tail asked
	 * for samples not yet emitted (it will pad) - the direct measure
	 * of an anchor that sits too new.
	 */
	int32_t margin_last;
	int32_t margin_min;
	int32_t margin_max;
	int32_t ref_unrel_hops; /* reference-unreliable fail-safe duck hangover
				 * (see AEC_SUP_REF_UNREL_HOPS): >0 => the window
				 * slid off the written ring, hold the deep duck */
	int32_t ref_skew_adj;   /* margin-servo: consumer-owned samples the read
				 * window is pulled earlier to cancel the emit-vs-
				 * mic clock skew (see AEC_MARGIN_SERVO). Reset on
				 * every re-anchor. Mic-thread-only, no race. */
	int32_t margin_anchor;  /* margin-servo target: margin captured at the
				 * last re-anchor (the epoch-chosen geometry) */
	int64_t last_feed_ms;   /* producer-side: uptime of the last tap feed */
	atomic_t feed_stamp_ms; /* 32-bit copy for the consumer's speaker-
				 * activity gate (atomic: torn-free across
				 * the ISR/thread boundary; 0 = never fed)
				 */
	atomic_t real_feed_stamp_ms; /* uptime ms of the last NON-SILENT reference
				 * feed (real speaker audio, gated on emitted power
				 * per AEC_REF_GATE_RMS). Distinct from feed_stamp_ms:
				 * the round-8 driver silence-feed keeps the session
				 * engaged between replies, so feed_stamp stays fresh
				 * when idle - only this stamp marks genuine echo-
				 * generating playback. Read by the suppressor to tell
				 * a live reference collapse (playback active, aligned
				 * window reads ~0) from true idle. 0 = none yet. */
	uint32_t feed_gaps;     /* producer-side: feed gaps > 2 block periods */
	uint32_t feed_gap_ms;   /* total duration of those gaps */

	/* physical-time pairing state: minimum-latency estimates of each
	 * stream's epoch (kernel uptime at which stream sample index 0
	 * occurred). Every observation (event time, cumulative samples)
	 * upper-bounds the epoch; the running minimum converges to it. The
	 * residual per-stream latency bias is constant and absorbed by the
	 * FIR taps.
	 */
	int64_t emit_epoch_ms;  /* producer-side, INT64_MAX until known */
	int64_t cap_epoch_ms;   /* consumer-side, INT64_MAX until known */
	int64_t emit_epoch_set_ms; /* when each epoch was (re)established, */
	int64_t cap_epoch_set_ms;  /* for the refinement freeze */
	uint32_t emit_refines;  /* applied refinements (establishment window) */
	uint32_t emit_refine_ms;
	uint32_t cap_refines;
	uint32_t cap_refine_ms;
	uint32_t emit_late;     /* post-freeze refinements, suppressed */
	uint32_t emit_late_ms;
	uint32_t cap_late;
	uint32_t cap_late_ms;
	/* capture-timeline loss accounting (see audio_aec_note_mic_loss)
	 * and the late-floor backstop for unreported losses: a two-bucket
	 * windowed minimum of (observation - epoch) lateness. Reported
	 * losses advance mic_total directly; unreported ones surface as a
	 * lateness floor that never returns to zero and slide the epoch.
	 */
	uint64_t mic_lost;
	uint32_t cap_slips;
	uint32_t cap_slip_ms;
	int32_t late_floor_cur;  /* min lateness, current bucket (ms) */
	int32_t late_floor_prev; /* min lateness, previous bucket (ms) */
	int64_t late_bucket_ms;  /* current bucket start, 0 = unarmed */
	/* emit-side twin (producer/ISR context only) */
	uint32_t emit_slips;
	uint32_t emit_slip_ms;
	int32_t emit_floor_cur;
	int32_t emit_floor_prev;
	int64_t emit_bucket_ms;
	uint32_t seg_start;     /* ring index where the current emission
				 * segment began; earlier indices hold a
				 * previous timeline and must read as silence
				 */
	uint64_t mic_total;     /* consumer-side mic samples this session */
	int64_t last_mic_ms;    /* consumer-side: last process call */

	/* load instrumentation (mic consumer thread) */
	uint32_t load_us_accum;
	uint32_t load_blocks;
	uint32_t overload_streak;

	/* clock-rate probes, one per stream (see audio_aec_stats) */
	struct aec_clkmon {
		uint64_t frames;
		uint64_t us;
		uint32_t runs;
		int64_t last_ms;
	} clkmon_mic, clkmon_ref;
} aec = {
	/* Off until a mic session opts in (frame.microphone.start{aec=true} or
	 * frame.microphone.aec(true)); the mic path is raw capture by default. */
	.enabled = ATOMIC_INIT(0),
	.ref_format_ok = true,
	.emit_epoch_ms = INT64_MAX,
	.cap_epoch_ms = INT64_MAX,
};

/* One-shot paired capture: a raw mic block and the exact raw reference
 * window it was cancelled against, so the host can cross-correlate them
 * and read the true echo delay in tap coordinates (aec('pair')). The
 * ~3.3KB is paid for from CONFIG_HALO_MEM_INTERNAL_SIZE.
 */
static int16_t pair_mic[AEC_MAX_BLOCK];
static int16_t pair_ref[AEC_TAPS + AEC_MAX_BLOCK];
static size_t pair_n;
static atomic_t pair_req;
static atomic_t pair_done;

/* one-block mic hold-back (see AEC_REF_LEAD): the echo's reference is
 * committed only at DMA completion, up to 20ms after the echo reached
 * the mic - each block is processed one block late so its window can
 * be served from real samples
 */
static int16_t held_blk[AEC_MAX_BLOCK];
static bool have_held;

#if defined(CONFIG_HALO_AUDIO_AEC_FDAF)
/* FDAF state (~50KB): spectra are in arm_rfft_fast_f32's packed layout
 * - [0]=DC, [1]=Nyquist, then re/im pairs for bins 1..FD_BINS-1. Owned
 * by the mic consumer thread.
 *
 * Placed in ITCM on device: DTCM is full (the Lua heap lives there),
 * and as TCM the ITCM is zero-wait-state for the FFT hot loops. NOTE
 * the ITCM is NOT otherwise free - halo_mem's "external SRAM" heap
 * (region 1, mic rings etc.) lives there too, config-addressed and so
 * invisible in the linker map: this state owns [0, EXTERNAL_SRAM_OFFSET)
 * and the heap starts above it (BUILD_ASSERTed below - a first attempt
 * that overlapped them cost a bricked-looking mic path). The generated
 * ITCM output section is NOLOAD, so sys_init must zero it explicitly - the
 * AEC can engage at any time after boot (a mic session opting in), and the
 * FDAF state must already be zero when it does.
 */
#if defined(CONFIG_CMSIS_DSP)
/* ITCM starts at address 0x0: the linker script collects the plain
 * "ITCM" input section before "ITCM.*", so this guard - the only thing
 * placed there - pins address 0 and a stray NULL-pointer write cannot
 * corrupt the filter state (which lives in ITCM.state, after it).
 * Symbol order WITHIN one input section is compiler-chosen, which is
 * why the guard cannot simply be the state struct's first member.
 */
static __used uint8_t null_guard[256] Z_GENERIC_SECTION(ITCM);
#define AEC_FD_SECTION Z_GENERIC_SECTION(ITCM.state)
#else
#define AEC_FD_SECTION /* host harness: ordinary bss */
#endif
static struct {
	float X[FD_K][FD_N];  /* raw ref spectra ring; slot = frame % FD_K */
	float Xf[FD_K][FD_N]; /* band-passed ref spectra ring (update view) */
	float W[FD_K][FD_N];  /* per-partition filter spectra */
	float P[FD_BINS];    /* smoothed per-bin ref power over the K parts */
	float Pi[FD_BINS];   /* this block's instantaneous per-bin power */
	float buf[FD_N];     /* time-domain scratch */
	float spec[FD_N];    /* spectrum scratch */
	float e_blk[FD_H];   /* post-clamp error block for the E transform */
	uint32_t frame;      /* hop counter */
} fd AEC_FD_SECTION;

#if defined(CONFIG_HALO_MEM_USE_EXTERNAL_SRAM) && \
	(CONFIG_HALO_MEM_EXTERNAL_SRAM_ADDR == 0)
BUILD_ASSERT(256 + sizeof(fd) <= CONFIG_HALO_MEM_EXTERNAL_SRAM_OFFSET,
	     "FDAF state must fit below the ITCM region-1 heap offset");
#endif

/* Residual-suppressor state (~9KB), sharing the FDAF's ITCM allotment
 * (sizeof(fd) + sizeof(sup) BUILD_ASSERTed under the region-1 heap
 * offset below). Memory-trimmed relative to the offline prototype, both
 * trims validated equal-or-better on the real captures:
 * - Sy is measured on the prediction spectrum Y the filter already
 *   computes (no y-history frame, no extra FFT);
 * - the gains are applied by direct time-domain convolution of the
 *   short kernel over res_hist (identical by construction to the
 *   constrained overlap-save product; no residual-spectrum buffer, two
 *   fewer FFTs).
 * Owned by the mic consumer thread.
 */
static struct {
	float res_hist[FD_N];   /* raw residual frames (the emitted audio) */
	float Sy[AEC_SUP_NBINS]; /* smoothed per-bin predicted-echo power */
	float Se[AEC_SUP_NBINS]; /* smoothed per-bin residual power */
	float g[AEC_SUP_NBINS];  /* attack/release-smoothed gains */
	float kc[AEC_SUP_KLEN];  /* rotated+windowed gain kernel */
	float kwin[AEC_SUP_KLEN];
	float gmin;              /* last block's min gain (diagnostics) */
	uint32_t onset;          /* hops of onset boost remaining (see ONSET_HOPS) */
	bool kwin_ready;
	/* envelope release gate (AEC_SUP_GCAP_ENV_GATE): fast/slow EMAs of the
	 * echo-removed excess residual, and the release hangover. */
	float gate_fast;         /* fast EMA of the excess residual */
	float gate_floor;        /* slow ambient-floor EMA (no freeze) */
	float gate_mid;          /* medium EMA: edge-detector baseline (see MID_A) */
	bool  gate_floor_init;   /* seed the floor/mid on the first block */
	uint32_t gate_hang;      /* release hangover blocks remaining */
	bool  gate_released;     /* last block's release decision (diagnostics) */
	bool  pb_hold;           /* last block the playback-active hold kept the cap
				  * that PREF_MIN would otherwise have lifted - the
				  * live reference-collapse guard (diagnostics) */
} sup AEC_FD_SECTION;
static atomic_t sup_enabled = ATOMIC_INIT(1);

#if defined(CONFIG_HALO_MEM_USE_EXTERNAL_SRAM) && \
	(CONFIG_HALO_MEM_EXTERNAL_SRAM_ADDR == 0)
BUILD_ASSERT(256 + sizeof(fd) + sizeof(sup) <=
	     CONFIG_HALO_MEM_EXTERNAL_SRAM_OFFSET,
	     "guard + FDAF + suppressor must fit below the ITCM heap offset");
#endif

#if defined(CONFIG_CMSIS_DSP)
static arm_rfft_fast_instance_f32 fd_fft;

static void aec_fft_init(void)
{
	/* the SIZE-SPECIFIC init: the generic arm_rfft_fast_init_f32
	 * references the twiddle/bit-reverse tables of EVERY supported
	 * size (32..4096), which linked ~160KB of dead const tables into
	 * the image; this pulls in the 1024-point set only
	 */
	arm_rfft_fast_init_1024_f32(&fd_fft);
}

/* in is scratch (destroyed); out = packed half spectrum */
static void aec_rfft(float *in, float *out)
{
	arm_rfft_fast_f32(&fd_fft, in, out, 0);
}

/* in = packed spectrum (destroyed); out = FD_N time samples, x1/N */
static void aec_irfft(float *in, float *out)
{
	arm_rfft_fast_f32(&fd_fft, in, out, 1);
}
#else /* host harness fallback */
/* Plain-C radix-2 FFT with the same packing and scaling conventions as
 * arm_rfft_fast_f32 (forward unscaled, inverse carries the 1/N), so the
 * host harness exercises numerically-equivalent code.
 */
#include <math.h>

static float fft_re[FD_N], fft_im[FD_N];
static float fft_tw_re[FD_N / 2], fft_tw_im[FD_N / 2];

static void aec_fft_init(void)
{
	for (int k = 0; k < FD_N / 2; k++) {
		fft_tw_re[k] = cosf(-2.0f * (float)M_PI * k / FD_N);
		fft_tw_im[k] = sinf(-2.0f * (float)M_PI * k / FD_N);
	}
}

static void host_cfft(int inv)
{
	for (int i = 1, j = 0; i < FD_N; i++) {
		int bit = FD_N >> 1;

		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			float t = fft_re[i]; fft_re[i] = fft_re[j]; fft_re[j] = t;
			t = fft_im[i]; fft_im[i] = fft_im[j]; fft_im[j] = t;
		}
	}
	for (int len = 2; len <= FD_N; len <<= 1) {
		int stride = FD_N / len;

		for (int i = 0; i < FD_N; i += len) {
			for (int k = 0; k < len / 2; k++) {
				float wr = fft_tw_re[k * stride];
				float wi = fft_tw_im[k * stride] * (inv ? -1.0f : 1.0f);
				int a = i + k, b = i + k + len / 2;
				float xr = fft_re[b] * wr - fft_im[b] * wi;
				float xi = fft_re[b] * wi + fft_im[b] * wr;

				fft_re[b] = fft_re[a] - xr;
				fft_im[b] = fft_im[a] - xi;
				fft_re[a] += xr;
				fft_im[a] += xi;
			}
		}
	}
}

static void aec_rfft(float *in, float *out)
{
	for (int i = 0; i < FD_N; i++) {
		fft_re[i] = in[i];
		fft_im[i] = 0.0f;
	}
	host_cfft(0);
	out[0] = fft_re[0];
	out[1] = fft_re[FD_N / 2];
	for (int b = 1; b < FD_BINS; b++) {
		out[2 * b] = fft_re[b];
		out[2 * b + 1] = fft_im[b];
	}
}

static void aec_irfft(float *in, float *out)
{
	fft_re[0] = in[0];
	fft_im[0] = 0.0f;
	fft_re[FD_N / 2] = in[1];
	fft_im[FD_N / 2] = 0.0f;
	for (int b = 1; b < FD_BINS; b++) {
		fft_re[b] = in[2 * b];
		fft_im[b] = in[2 * b + 1];
		fft_re[FD_N - b] = in[2 * b];
		fft_im[FD_N - b] = -in[2 * b + 1];
	}
	host_cfft(1);
	for (int i = 0; i < FD_N; i++) {
		out[i] = fft_re[i] * (1.0f / FD_N);
	}
}
#endif /* CONFIG_CMSIS_DSP */
#endif /* CONFIG_HALO_AUDIO_AEC_FDAF */

/* Min-latency epoch estimator with a freeze: every observation
 * upper-bounds the epoch and the running minimum converges to it, but
 * refinements are only applied within AEC_EPOCH_FREEZE_MS of the epoch
 * being (re)established - afterwards they are counted and suppressed.
 */
static void epoch_observe(int64_t *epoch, int64_t *set_ms, int64_t obs,
			  int64_t now_ms, uint32_t *refines,
			  uint32_t *refine_ms, uint32_t *late,
			  uint32_t *late_ms)
{
	if (*epoch == INT64_MAX) {
		*epoch = obs;
		*set_ms = now_ms;
	} else if (obs < *epoch) {
		if ((now_ms - *set_ms) <= AEC_EPOCH_FREEZE_MS) {
			*refines += 1;
			*refine_ms += (uint32_t)(*epoch - obs);
			*epoch = obs;
		} else {
			*late += 1;
			*late_ms += (uint32_t)(*epoch - obs);
		}
	}
}

/* Accumulate frames against the kernel clock over contiguous activity: a
 * gap longer than this since the previous event starts a new run and
 * contributes neither frames nor time, so frames/us measures the stream's
 * true sample rate across start/stop gaps and FIFO stalls.
 */
#define AEC_CLKMON_GAP_MS 200

static void clkmon_note(struct aec_clkmon *m, size_t frames)
{
	int64_t now_ms = k_uptime_get();

	/* Accumulate kernel-uptime deltas, not cycle-counter deltas: the
	 * cycle counter loses time across idle sleep (RTC idle timer), which
	 * skews rates by hundreds of ppm depending on where events land in
	 * the sleep/wake cycle. Millisecond quantization telescopes away
	 * within a contiguous run.
	 */
	if (m->last_ms != 0 && (now_ms - m->last_ms) <= AEC_CLKMON_GAP_MS) {
		m->us += (uint64_t)(now_ms - m->last_ms) * 1000u;
		m->frames += frames;
	} else {
		m->runs++;
	}
	m->last_ms = now_ms;
}

void audio_aec_enable(bool enable)
{
	if (enable) {
		/* start from a clean filter: adaptation is fast (<1s) and a
		 * stale/diverged state is worse than a cold start
		 */
		memset(aec.w, 0, sizeof(aec.w));
		memset(aec.x, 0, sizeof(aec.x));
		memset(aec.xf, 0, sizeof(aec.xf));
#if defined(CONFIG_HALO_AUDIO_AEC_FDAF)
		memset(fd.X, 0, sizeof(fd.X));
		memset(fd.Xf, 0, sizeof(fd.Xf));
		memset(fd.W, 0, sizeof(fd.W));
		memset(fd.P, 0, sizeof(fd.P));
		fd.frame = 0;
		aec.fd_hist_fill = 0;
		/* W wiped -> cold-onset hot-mu schedule re-arms */
		aec.mu_hot_excess = AEC_FD_MU_HOT_EXCESS;
		memset(sup.res_hist, 0, sizeof(sup.res_hist));
		memset(sup.Sy, 0, sizeof(sup.Sy));
		memset(sup.Se, 0, sizeof(sup.Se));
		for (size_t j = 0; j < AEC_SUP_NBINS; j++) {
			sup.g[j] = 1.0f;
		}
		sup.gmin = 1.0f;
#else
		aec.xf_norm2 = 0.0f;
#endif
		aec.p_ref = 0.0f;
		aec.p_err = 0.0f;
		aec.p_mic = 0.0f;
		aec.mic_lf1 = 0.0f;
		aec.mic_lf2 = 0.0f;
		aec.ref_lf1 = 0.0f;
		aec.ref_lf2 = 0.0f;
		aec.err_lf1 = 0.0f;
		aec.err_lf2 = 0.0f;
		memset(aec.ref_lp, 0, sizeof(aec.ref_lp));
		memset(aec.err_lp, 0, sizeof(aec.err_lp));
		aec.ref_pe = 0.0f;
		aec.err_pe = 0.0f;
		have_held = false;
		aec.gate_hangover = 0;
		aec.mu_ramp = 0;
		aec.overload_streak = 0;
		/* per-session diagnostics start fresh */
		aec.ref_underruns = 0;
		aec.ref_pads = 0;
		aec.feed_gaps = 0;
		aec.feed_gap_ms = 0;
		aec.resyncs = 0;
		aec.emit_refines = 0;
		aec.emit_refine_ms = 0;
		aec.cap_refines = 0;
		aec.cap_refine_ms = 0;
		aec.emit_late = 0;
		aec.emit_late_ms = 0;
		aec.cap_late = 0;
		aec.cap_late_ms = 0;
		aec.margin_last = 0;
		aec.margin_min = INT32_MAX;
		aec.margin_max = INT32_MIN;
		aec.mic_lost = 0;
		aec.cap_slips = 0;
		aec.cap_slip_ms = 0;
		aec.emit_slips = 0;
		aec.emit_slip_ms = 0;
#if AEC_MARGIN_SERVO
		aec.ref_skew_adj = 0;
#endif
	}
	atomic_set(&aec.enabled, enable ? 1 : 0);
	if (!enable) {
		aec.synced = false;
	}
	LOG_INF("AEC %s", enable ? "enabled" : "disabled");
}

bool audio_aec_is_enabled(void)
{
	return atomic_get(&aec.enabled) != 0;
}

void audio_aec_suppress(bool enable)
{
#if defined(CONFIG_HALO_AUDIO_AEC_FDAF)
	if (enable && atomic_get(&sup_enabled) == 0) {
		/* stale spectral state must not gate the fresh engage */
		memset(sup.res_hist, 0, sizeof(sup.res_hist));
		memset(sup.Sy, 0, sizeof(sup.Sy));
		memset(sup.Se, 0, sizeof(sup.Se));
		for (size_t j = 0; j < AEC_SUP_NBINS; j++) {
			sup.g[j] = 1.0f;
		}
		sup.onset = AEC_SUP_ONSET_HOPS;   /* boost from this re-enable */
		/* envelope gate: seed the floor fresh on re-engage */
		sup.gate_fast = 0.0f;
		sup.gate_floor = 0.0f;
		sup.gate_floor_init = false;
		sup.gate_hang = 0;
		sup.gate_released = false;
	}
	atomic_set(&sup_enabled, enable ? 1 : 0);
	LOG_INF("AEC suppressor %s", enable ? "enabled" : "disabled");
#else
	(void)enable;
#endif
}

bool audio_aec_is_suppressed(void)
{
#if defined(CONFIG_HALO_AUDIO_AEC_FDAF)
	return atomic_get(&sup_enabled) != 0;
#else
	return false;
#endif
}

void audio_aec_feed_reference(const int16_t *pcm, size_t samples,
			      uint32_t sample_rate, uint8_t channels)
{
	/* rate probe counts every handed block, independent of AEC state */
	clkmon_note(&aec.clkmon_ref,
		    (channels == 2) ? samples / 2 : samples);

	if (atomic_get(&aec.enabled) == 0) {
		return;
	}

	/* The I2S session runs stereo on Halo (one speaker per arm) even for
	 * mono Lua playback - the same content on both channels. Downmix a
	 * stereo reference to mono; the per-arm difference is small compared
	 * to what the adaptive filter absorbs.
	 */
	if (sample_rate != AEC_SAMPLE_RATE || channels < 1 || channels > 2) {
		/* can't cancel against this session; note it once */
		if (!aec.warned_ref_format) {
			aec.warned_ref_format = true;
			LOG_WRN("AEC bypassed: speaker session %u Hz %u ch "
				"(need %u Hz, 1-2 ch)",
				sample_rate, channels, AEC_SAMPLE_RATE);
		}
		aec.ref_format_ok = false;
		return;
	}
	aec.ref_format_ok = true;
	aec.warned_ref_format = false;

	/* Emission resumed after a gap (DMA queue drained - BLE feed stall,
	 * app pause, fresh session): whatever the consumer was anchored to
	 * is stale, so drop the sync, re-learn the emission epoch and fence
	 * off the previous segment's ring content. Benign
	 * producer->consumer race on a bool; the consumer re-anchors within
	 * one block either way.
	 */
	int64_t now_ms = k_uptime_get();
	int64_t gap = now_ms - aec.last_feed_ms;

	if (aec.last_feed_ms != 0 && gap > 40) {
		aec.feed_gaps++;
		aec.feed_gap_ms += (uint32_t)gap;
		aec.synced = false;
		aec.emit_epoch_ms = INT64_MAX;
		aec.seg_start = (uint32_t)atomic_get(&aec.ref_widx);
		aec.emit_bucket_ms = 0;
	}
	aec.last_feed_ms = now_ms;
	/* uptime 0 means "never fed"; a real feed in the boot millisecond
	 * just waits one more block to engage
	 */
	if ((uint32_t)now_ms != 0) {
		atomic_set(&aec.feed_stamp_ms, (atomic_val_t)(uint32_t)now_ms);
	}

	/* lock-free SPSC write; producer is the speaker driver's DMA
	 * completion callback, so this block JUST FINISHED emitting -
	 * the ring index and the epoch estimate below are emission-exact
	 * regardless of the DMA queue occupancy
	 */
	uint32_t w = (uint32_t)atomic_get(&aec.ref_widx);
	size_t frames = (channels == 2) ? samples / 2 : samples;

	if (frames > AEC_MAX_BLOCK) {
		frames = AEC_MAX_BLOCK;
	}
	int64_t ss = 0; /* sum of squares of the (downmixed) samples written */

	if (channels == 2) {
		for (size_t i = 0; i < frames; i++) {
			int16_t v = (int16_t)(((int32_t)pcm[2 * i] +
					       (int32_t)pcm[2 * i + 1]) / 2);

			aec.ref_ring[(w + i) % REF_RING_SAMPLES] = v;
			ss += (int64_t)v * v;
		}
	} else {
		for (size_t i = 0; i < frames; i++) {
			int16_t v = pcm[i];

			aec.ref_ring[(w + i) % REF_RING_SAMPLES] = v;
			ss += (int64_t)v * v;
		}
	}
	atomic_set(&aec.ref_widx, (atomic_val_t)(w + frames));

	/* stamp genuine (non-silent) playback: only real emitted audio generates
	 * echo, so gate the stamp on block power at the same threshold the
	 * adaptation gate uses (AEC_REF_GATE_RMS). The round-8 driver silence-
	 * feed (all zeros) never trips it, so this stays stale through idle even
	 * though feed_stamp_ms above keeps refreshing. Compile-time-const
	 * threshold => no runtime float in the tap ISR. */
	if (frames > 0 && (uint32_t)now_ms != 0) {
		const int32_t rms_cnt = (int32_t)(AEC_REF_GATE_RMS * 32768.0f);

		if (ss / (int64_t)frames > (int64_t)rms_cnt * rms_cnt) {
			atomic_set(&aec.real_feed_stamp_ms,
				   (atomic_val_t)(uint32_t)now_ms);
		}
	}

	if (frames > 0) {
		/* ring index w+frames was emitted ~now */
		int64_t e = now_ms - (int64_t)((w + (uint32_t)frames) / 16u);

		epoch_observe(&aec.emit_epoch_ms, &aec.emit_epoch_set_ms, e,
			      now_ms, &aec.emit_refines, &aec.emit_refine_ms,
			      &aec.emit_late, &aec.emit_late_ms);

		/* emit-side late-floor (see AEC_EMIT_FLOOR_MS): unflagged
		 * emission stretching (TX FIFO dry spells) makes these
		 * observations run persistently late; slide the epoch by
		 * the windowed-min so the write head's timeline stays true
		 */
		if (aec.emit_bucket_ms == 0) {
			aec.emit_bucket_ms = now_ms;
			aec.emit_floor_cur = INT32_MAX;
			aec.emit_floor_prev = INT32_MAX;
		}

		int32_t elate = (int32_t)(e - aec.emit_epoch_ms);

		if (elate < aec.emit_floor_cur) {
			aec.emit_floor_cur = elate;
		}
		if ((now_ms - aec.emit_bucket_ms) >= AEC_LATE_FLOOR_WIN_MS) {
			int32_t floor = MIN(aec.emit_floor_cur,
					    aec.emit_floor_prev);

			if (floor != INT32_MAX && floor > AEC_EMIT_FLOOR_MS) {
				int32_t slide = floor - AEC_EMIT_FLOOR_MS / 2;

				aec.emit_epoch_ms += slide;
				aec.emit_epoch_set_ms = now_ms;
				aec.emit_slips++;
				aec.emit_slip_ms += (uint32_t)slide;
				aec.emit_floor_prev = INT32_MAX;
			} else {
				aec.emit_floor_prev = aec.emit_floor_cur;
			}
			aec.emit_floor_cur = INT32_MAX;
			aec.emit_bucket_ms = now_ms;
		}
	}
}

/* Pull exactly n reference samples into dst (as float / 32768), padding
 * with silence where the ring cannot serve, anchored by PHYSICAL TIME:
 * the window ends where emission time equals this mic block's capture
 * time (minus the one-block feed-quantization bias). Anchoring by FIFO
 * occupancy instead - "newest at consume time" - was the alignment
 * killer: the mic driver queue carries a variable 0..160ms backlog, so
 * a consume-time anchor places the window up to 160ms away from the
 * echo, often entirely outside it. Both stream epochs are min-latency
 * estimates maintained by the producer and consumer (see
 * audio_aec_feed_reference / audio_aec_process).
 * dst_f receives the same samples through the streaming high-pass used
 * by the filtered-error update (state continues across blocks and pads).
 * Returns how many were real ring samples (the rest is padding).
 */
static size_t ref_consume(float *dst, float *dst_f, size_t n)
{
	uint32_t w = (uint32_t)atomic_get(&aec.ref_widx);
	uint32_t r = (uint32_t)atomic_get(&aec.ref_ridx);
#if AEC_MARGIN_SERVO
	int32_t servo_dr = 0; /* margin-servo read-advance trim this block */
#endif
	size_t take = 0;

	if (aec.cap_epoch_ms == INT64_MAX || aec.emit_epoch_ms == INT64_MAX) {
		/* one of the streams hasn't established its epoch yet (no
		 * emission, or mic just started): the reference is silence
		 */
		aec.synced = false;
	} else {
		/* ring index whose emission time equals this block's capture
		 * end time, biased one block back so the tap's block-granular
		 * feeds can't leave the newest part of the window unserved
		 * (the bias is a constant echo-depth offset the taps absorb)
		 */
		int64_t cap_end_ms = aec.cap_epoch_ms +
				     (int64_t)(aec.mic_total / 16u);
		uint32_t desired_end =
			(uint32_t)((cap_end_ms - aec.emit_epoch_ms) * 16) -
			AEC_REF_LEAD;
#if AEC_MARGIN_SERVO
		/* pull the window earlier by the accumulated skew correction so
		 * the drift check, margin, and read all see one coherent anchor */
		desired_end -= (uint32_t)aec.ref_skew_adj;
#endif
		int32_t drift = (int32_t)(desired_end - (r + (uint32_t)n));

		if (!aec.synced || drift > 160 || drift < -160) {
			/* first anchor, or the pairing walked (mic session
			 * restart, emission gap, timestamp refinement):
			 * re-anchor and clear the reference history - the
			 * old history belongs to the pre-jump timeline and
			 * splicing it against the new window makes the
			 * filter output a burst while it re-converges
			 */
			if (aec.synced) {
				aec.resyncs++;
				LOG_DBG("AEC ref resync #%u (drift %d)",
					aec.resyncs, drift);
#if defined(CONFIG_HALO_AUDIO_AEC_FDAF)
				/* re-arm the onset duck to cover the prediction
				 * collapse this history wipe causes (see
				 * AEC_SUP_RESYNC_HOPS): the blanket ceiling ducks
				 * the uplink through the refill burst regardless
				 * of the momentarily-zero Sy. Only genuine
				 * resyncs (aec.synced), not the first anchor - a
				 * first anchor's onset is the reply-gate trigger.
				 * MAX so a resync mid-reply keeps the reply's own
				 * longer onset window.
				 */
				if (sup.onset < AEC_SUP_RESYNC_HOPS) {
					sup.onset = AEC_SUP_RESYNC_HOPS;
				}
#endif
			}
			memset(aec.x, 0, sizeof(aec.x));
			memset(aec.xf, 0, sizeof(aec.xf));
#if defined(CONFIG_HALO_AUDIO_AEC_FDAF)
			memset(fd.X, 0, sizeof(fd.X));
			memset(fd.Xf, 0, sizeof(fd.Xf));
			memset(fd.P, 0, sizeof(fd.P));
			aec.fd_hist_fill = 0;
#else
			aec.xf_norm2 = 0.0f;
#endif
			aec.mu_ramp = 0;
#if AEC_MARGIN_SERVO
			/* re-anchor to the RAW epoch position (undo the skew
			 * correction) so r and a zeroed ref_skew_adj are mutually
			 * consistent - otherwise the next block's drift jumps by
			 * the old correction and the warm filter sees a shifted
			 * window (the re-engage misalignment). This makes the
			 * re-anchor identical to the servo-off path; capture the
			 * clean geometry as the servo target. */
			uint32_t raw_end = desired_end + (uint32_t)aec.ref_skew_adj;

			/* never RE-anchor an established stream with the window
			 * at/past the write head (see AEC_MARGIN_RESYNC_FLOOR):
			 * pull it back to a positive floor so the held margin
			 * can't latch the ref-unreliable duck and blanket-mute
			 * both ends. Only on a resync (aec.synced) - the first
			 * anchor establishes the trusted geometry (device-healthy
			 * +75..+187, host ~0) and is taken as-is. */
			if (aec.synced &&
			    (int32_t)(w - raw_end) < AEC_MARGIN_RESYNC_FLOOR) {
				raw_end = w - (uint32_t)AEC_MARGIN_RESYNC_FLOOR;
			}

			r = raw_end - (uint32_t)n;
			aec.margin_anchor = (int32_t)(w - raw_end);
			aec.ref_skew_adj = 0;
#else
			r = desired_end - (uint32_t)n;
#endif
			aec.synced = true;
		}

		int32_t margin = (int32_t)(w - (r + (uint32_t)n));

		aec.margin_last = margin;
		if (margin < aec.margin_min) {
			aec.margin_min = margin;
		}
		if (margin > aec.margin_max) {
			aec.margin_max = margin;
		}

#if AEC_MARGIN_SERVO
		/* Hold margin at the anchor geometry, cancelling the emit-vs-mic
		 * clock skew. servo_dr reduces (slip back, read slower -> margin
		 * up) or increases (catch up -> margin down) this block's read-
		 * pointer advance by <= AEC_MARGIN_SERVO_STEP; ref_skew_adj (also
		 * subtracted from desired_end above) moves in lock-step so `drift`
		 * is invariant and no resync is provoked. Deadband ignores per-
		 * block jitter so only the persistent skew drives it. */
		if (aec.synced) {
			int32_t err = aec.margin_anchor - margin;

			if (err > AEC_MARGIN_DEADBAND) {
				/* window drifted too new -> read slower */
				servo_dr = MIN(err - AEC_MARGIN_DEADBAND,
					       AEC_MARGIN_SERVO_STEP);
			} else if (err < -AEC_MARGIN_DEADBAND &&
				   aec.ref_skew_adj > 0) {
				/* drifted too old and we have slip to give back */
				servo_dr = -MIN(-err - AEC_MARGIN_DEADBAND,
						AEC_MARGIN_SERVO_STEP);
				servo_dr = MAX(servo_dr, -aec.ref_skew_adj);
			}
			aec.ref_skew_adj += servo_dr;
		}
#endif

		/* reference-unreliable fail-safe (see AEC_SUP_REF_UNREL_HOPS):
		 * margin < 0 means this block's window tail is past the write
		 * head - the reference is padding, not real emission, so the
		 * residual will be uncancelled echo. Arm/refresh the duck
		 * hangover; the suppressor holds the ceiling on and deepens.
		 * FDAF-only (the residual suppressor lives in the FDAF path).
		 */
#if defined(CONFIG_HALO_AUDIO_AEC_FDAF)
		if (margin < 0) {
			aec.ref_unrel_hops = AEC_SUP_REF_UNREL_HOPS;
		} else if (aec.ref_unrel_hops > 0) {
			aec.ref_unrel_hops--;
		}
#endif

		/* serve what the current segment actually holds:
		 * [max(seg_start, w - RING), w); pad silence outside it
		 * (head: pre-segment - the gap really was silence - or
		 * overwritten; tail: emission genuinely behind the capture
		 * timeline - a real pause, where silence IS the reference)
		 */
		uint32_t seg = aec.seg_start;

		for (size_t i = 0; i < n; i++) {
			uint32_t idx = r + (uint32_t)i;

			if ((int32_t)(w - idx) > 0 &&
			    (int32_t)(w - idx) <= (int32_t)REF_RING_SAMPLES &&
			    (int32_t)(idx - seg) >= 0) {
				dst[i] = (float)aec.ref_ring[idx % REF_RING_SAMPLES] *
					 (1.0f / 32768.0f);
				take++;
			} else {
				dst[i] = 0.0f;
			}
		}
	}

	if (!aec.synced) {
		for (size_t i = 0; i < n; i++) {
			dst[i] = 0.0f;
		}
	}
	if (aec.synced && take < n) {
		aec.ref_underruns++;
		aec.ref_pads += n - take;
	}
#if AEC_MARGIN_SERVO
	/* trim the advance by the servo step: reading `servo_dr` fewer samples
	 * (slip back) re-reads them next block, cancelling the skew. Matches the
	 * ref_skew_adj applied to desired_end so drift stays invariant. */
	atomic_set(&aec.ref_ridx, (atomic_val_t)(r + n - servo_dr));
#else
	atomic_set(&aec.ref_ridx, (atomic_val_t)(r + n));
#endif

	/* filtered update view - only the time-domain build wants it (the
	 * FDAF's band selection is per-bin and passes dst_f = NULL)
	 */
	if (dst_f) {
		for (size_t i = 0; i < n; i++) {
			aec.ref_lf1 += AEC_HPF_ALPHA * (dst[i] - aec.ref_lf1);

			float t = dst[i] - aec.ref_lf1;

			aec.ref_lf2 += AEC_HPF_ALPHA * (t - aec.ref_lf2);
			t -= aec.ref_lf2;
			/* band-limit from above too (see AEC_LPF_ALPHA) */
			for (int s = 0; s < 3; s++) {
				aec.ref_lp[s] += AEC_LPF_ALPHA * (t - aec.ref_lp[s]);
				t = aec.ref_lp[s];
			}
			/* pre-emphasis (see AEC_PREEMPH): whitens speech's
			 * spectral tilt in the update domain; same filter on
			 * the error view
			 */
			dst_f[i] = t - AEC_PREEMPH * aec.ref_pe;
			aec.ref_pe = t;
		}
	}

	return take;
}

#if defined(CONFIG_HALO_AUDIO_AEC_FDAF)
/* One 20ms hop of the per-bin canceller (see the FDAF block comment at
 * the top). aec.x has already been slid and its newest FD_H samples
 * filled by ref_consume; pcm is the (held) mic block, cancelled in
 * place. Returns whether the filter adapted this block.
 */
static bool fd_process_block(int16_t *pcm, bool adapt_ok)
{
	/* idempotent; belt-and-braces for hosts without SYS_INIT (the
	 * harness) - on device sys_init already did it
	 */
	static bool fft_ready;

	if (!fft_ready) {
		aec_fft_init();
		fft_ready = true;
	}

	uint32_t slot = fd.frame % FD_K;

	/* newest reference frames: raw (for the echo estimate) and
	 * band-passed (for the gradient - see the xf comment in the state
	 * struct: update views must be filtered BEFORE the frame gating)
	 */
	memcpy(fd.buf, aec.x + (AEC_TAPS + AEC_MAX_BLOCK) - FD_N,
	       FD_N * sizeof(float));
	aec_rfft(fd.buf, fd.X[slot]);
	memcpy(fd.buf, aec.xf + (AEC_TAPS + AEC_MAX_BLOCK) - FD_N,
	       FD_N * sizeof(float));
	aec_rfft(fd.buf, fd.Xf[slot]);

	/* predicted echo spectrum Y = sum_k W_k . X_{m-k} */
	float *Y = fd.spec;

	memset(Y, 0, FD_N * sizeof(float));
	for (uint32_t k = 0; k < FD_K; k++) {
		const float *X = fd.X[(fd.frame + FD_K - k) % FD_K];
		const float *W = fd.W[k];

		Y[0] += W[0] * X[0];
		Y[1] += W[1] * X[1];
		for (uint32_t b = 1; b < FD_BINS; b++) {
			float xr = X[2 * b], xi = X[2 * b + 1];
			float wr = W[2 * b], wi = W[2 * b + 1];

			Y[2 * b] += wr * xr - wi * xi;
			Y[2 * b + 1] += wr * xi + wi * xr;
		}
	}

	/* suppressor's predicted-echo power, measured on Y before the
	 * IFFT destroys it (see the sup struct comment: validated against
	 * an exact prediction-history frame offline)
	 */
	if (atomic_get(&sup_enabled) != 0) {
		for (uint32_t b = FD_BIN_LO; b <= FD_BIN_HI; b++) {
			float p = Y[2 * b] * Y[2 * b] +
				  Y[2 * b + 1] * Y[2 * b + 1];

			sup.Sy[b - FD_BIN_LO] +=
				AEC_SUP_POW_A * (p - sup.Sy[b - FD_BIN_LO]);
		}
	}

	aec_irfft(Y, fd.buf); /* last FD_H samples are this block's y */

	const float *y_t = fd.buf + FD_N - FD_H;

	/* subtract per sample, with the same high-passed-power echo
	 * sanity clamp as the time-domain build
	 */
	for (uint32_t i = 0; i < FD_H; i++) {
		float y = y_t[i];
		float d = (float)pcm[i] * (1.0f / 32768.0f);

		aec.mic_lf1 += AEC_HPF_ALPHA * (d - aec.mic_lf1);

		float d_hp1 = d - aec.mic_lf1;

		aec.mic_lf2 += AEC_HPF_ALPHA * (d_hp1 - aec.mic_lf2);

		float d_hp = d_hp1 - aec.mic_lf2;

		aec.p_mic += AEC_POW_ALPHA * (d_hp * d_hp - aec.p_mic);
		if (y * y > AEC_Y_CLAMP_P * aec.p_mic) {
			float lim = __builtin_sqrtf(AEC_Y_CLAMP_P * aec.p_mic);

			y = (y > 0.0f) ? lim : -lim;
		}

		float e = d - y;
		float out = e * 32768.0f;

		if (out > 32767.0f) {
			out = 32767.0f;
		} else if (out < -32768.0f) {
			out = -32768.0f;
		}
		pcm[i] = (int16_t)out;

		/* streaming band-pass of the error for the update, the
		 * same cascade ref_consume ran on the xf view - matched
		 * views keep the per-bin fixed point unbiased, and a
		 * STREAMING filter (state across blocks) removes the
		 * rumble before the frame gating can leak it in-band
		 */
		aec.err_lf1 += AEC_HPF_ALPHA * (e - aec.err_lf1);

		float e_f1 = e - aec.err_lf1;

		aec.err_lf2 += AEC_HPF_ALPHA * (e_f1 - aec.err_lf2);

		float e_f = e_f1 - aec.err_lf2;

		for (int s = 0; s < 3; s++) {
			aec.err_lp[s] += AEC_LPF_ALPHA * (e_f - aec.err_lp[s]);
			e_f = aec.err_lp[s];
		}
		fd.e_blk[i] = e_f;
	}

	/* error spectrum: e in the frame's last FD_H slots, zeros before
	 * (keeps the gradient's first FD_N-FD_H lags linear, so the
	 * constraint projection below is alias-free)
	 */
	memset(fd.buf, 0, (FD_N - FD_H) * sizeof(float));
	memcpy(fd.buf + FD_N - FD_H, fd.e_blk, FD_H * sizeof(float));

	float *E = fd.spec;

	aec_rfft(fd.buf, E);

	/* per-bin ref power (over all K partitions) and the in-band block
	 * powers for the double-talk detector
	 */
	float pr = 0.0f, pe = 0.0f, psum = 0.0f;
	const float *Xn = fd.Xf[slot];

	for (uint32_t b = FD_BIN_LO; b <= FD_BIN_HI; b++) {
		float inst = 0.0f;

		for (uint32_t k = 0; k < FD_K; k++) {
			const float *X = fd.Xf[(fd.frame + FD_K - k) % FD_K];

			inst += X[2 * b] * X[2 * b] +
				X[2 * b + 1] * X[2 * b + 1];
		}
		fd.Pi[b] = inst;
		fd.P[b] += AEC_FD_POW_ALPHA * (inst - fd.P[b]);
		psum += fd.P[b];
		pr += Xn[2 * b] * Xn[2 * b] + Xn[2 * b + 1] * Xn[2 * b + 1];
		pe += E[2 * b] * E[2 * b] + E[2 * b + 1] * E[2 * b + 1];
	}
	aec.p_ref += AEC_FD_POW_ALPHA * (pr - aec.p_ref);
	aec.p_err += AEC_FD_POW_ALPHA * (pe - aec.p_err);

#if AEC_SUP_GCAP_ENV_GATE
	/* near-end release gate (see AEC_SUP_GCAP_ENV_GATE): echo-removed
	 * excess residual, slow-floor onset detector. Runs on the smoothed
	 * in-band powers just updated above. Sets sup.gate_released, consumed
	 * by the steady-cap block below.
	 */
	{
		float re = __builtin_sqrtf(aec.p_err > 0.0f ? aec.p_err : 0.0f);
		float xr = __builtin_sqrtf(aec.p_ref > 0.0f ? aec.p_ref : 0.0f);
		float ex = re - AEC_SUP_GATE_KAPPA * xr;

		if (ex < 0.0f) {
			ex = 0.0f;
		}
		sup.gate_fast += AEC_SUP_GATE_FAST_A * (ex - sup.gate_fast);
		if (!sup.gate_floor_init) {
			sup.gate_floor = ex;
			sup.gate_mid = ex;
			sup.gate_floor_init = true;
		}
		sup.gate_floor += AEC_SUP_GATE_FLOOR_A * (ex - sup.gate_floor);

		/* sustained level test: echo-latency-bound (needs near-end to
		 * out-power the echo-contaminated floor) - noise-robust hold. */
		bool level = sup.gate_fast >
				     AEC_SUP_GATE_RATIO * (sup.gate_floor + 1e-9f) &&
			     (sup.gate_fast - sup.gate_floor) > AEC_SUP_GATE_ABSFLOOR;
		/* rising-edge test: fires in ~1-3 hops on a voice onset regardless
		 * of the steady echo level (see AEC_SUP_GATE_EDGE_ABS) - the fast
		 * near-end release. Read against the PREVIOUS baseline, then fold
		 * this block into mid below. */
		bool edge = (sup.gate_fast - sup.gate_mid) > AEC_SUP_GATE_EDGE_ABS &&
			    sup.gate_fast >
				    AEC_SUP_GATE_EDGE_RATIO * (sup.gate_mid + 1e-9f);
		sup.gate_mid += AEC_SUP_GATE_MID_A * (ex - sup.gate_mid);
		bool active = level || edge;

		if (active) {
			sup.gate_hang = AEC_SUP_GATE_HANG;
		} else if (sup.gate_hang > 0) {
			sup.gate_hang--;
		}
		sup.gate_released = active || sup.gate_hang > 0;
	}
#endif

	/* history-fill guard (see FD_HIST_FILL_HOPS): no adaptation on
	 * frames still containing a wipe's gating edge
	 */
	bool hist_ok = aec.fd_hist_fill >= FD_HIST_FILL_HOPS;

	if (!hist_ok) {
		aec.fd_hist_fill++;
	}

	bool adapt = adapt_ok && hist_ok && aec.p_ref > 1e-8f &&
		     aec.p_err < AEC_DTD_THRESHOLD * aec.p_ref;

	if (adapt) {
		/* cold-onset schedule (see AEC_FD_MU_HOT_EXCESS): hot mu
		 * settling exponentially, replacing the linear soft-start.
		 * Only enable() re-arms the excess - resyncs and idle gaps
		 * keep W, so warm re-engages adapt at plain MU from the
		 * first block.
		 */
		float mu = AEC_FD_MU + aec.mu_hot_excess;

		aec.mu_hot_excess *= AEC_FD_MU_SETTLE;

		float eps = AEC_FD_EPS_REL * psum /
			    (float)(FD_BIN_HI - FD_BIN_LO + 1) + 1e-12f;

		for (uint32_t b = FD_BIN_LO; b <= FD_BIN_HI; b++) {
			/* normalize by the LARGER of the smoothed and the
			 * instantaneous per-bin power: a smoothed-only
			 * denominator lags amplitude modulation (speech
			 * envelopes, playback onsets after the smoother
			 * decayed) and over-steps into divergence on every
			 * rise; the max bounds the per-bin step at the NLMS
			 * stability point while keeping the smoothed
			 * behaviour through decays
			 */
			float p = MAX(fd.P[b], fd.Pi[b]);
			float g = mu / (p + eps);
			float er = E[2 * b], ei = E[2 * b + 1];

			for (uint32_t k = 0; k < FD_K; k++) {
				const float *X = fd.Xf[(fd.frame + FD_K - k) % FD_K];
				float *W = fd.W[k];
				float xr = X[2 * b], xi = X[2 * b + 1];

				W[2 * b] = W[2 * b] * AEC_FD_LEAK +
					   g * (xr * er + xi * ei);
				W[2 * b + 1] = W[2 * b + 1] * AEC_FD_LEAK +
					       g * (xr * ei - xi * er);
			}
		}
	}

	/* constraint projection, round-robin one partition per block:
	 * removes the unconstrained blocks' circular-gradient leakage and
	 * exports the partition's time-domain taps for diagnostics
	 * (AEC_FD_CONSTRAIN_ALL: every partition every block, for host A/B)
	 */
#ifdef AEC_FD_CONSTRAIN_ALL
	for (uint32_t c = 0; c < FD_K; c++) {
#else
	uint32_t c = slot;
	{
#endif
		memcpy(fd.spec, fd.W[c], FD_N * sizeof(float));
		aec_irfft(fd.spec, fd.buf);
		memset(fd.buf + FD_H, 0, (FD_N - FD_H) * sizeof(float));
		for (uint32_t t = 0; t < FD_H; t++) {
			aec.w[AEC_TAPS - 1 - (c * FD_H + t)] = fd.buf[t];
		}
		aec_rfft(fd.buf, fd.W[c]);
	}

	/* divergence guard on the spectral (== time-domain, Parseval)
	 * filter norm, mirroring the time-domain build's rescale
	 */
	float wn = 0.0f;

	for (uint32_t k = 0; k < FD_K; k++) {
		const float *W = fd.W[k];
		float s = W[0] * W[0] + W[1] * W[1];

		for (uint32_t b = 1; b < FD_BINS; b++) {
			s += 2.0f * (W[2 * b] * W[2 * b] +
				     W[2 * b + 1] * W[2 * b + 1]);
		}
		wn += s * (1.0f / FD_N);
	}
	if (wn > AEC_NORM_CLAMP) {
		float s = __builtin_sqrtf((AEC_NORM_CLAMP / 4.0f) / wn);

		for (uint32_t k = 0; k < FD_K; k++) {
			for (uint32_t j = 0; j < FD_N; j++) {
				fd.W[k][j] *= s;
			}
		}
		for (uint32_t j = 0; j < AEC_TAPS; j++) {
			aec.w[j] *= s;
		}
		if ((aec.norm_clamps++ % 100) == 0) {
			LOG_WRN("AEC norm clamp #%u (%.1f)",
				aec.norm_clamps, (double)wn);
		}
	}

	/* ---- residual suppressor (see AEC_SUP_BETA): output path only,
	 * the adaptation above never sees it. Per-bin gains from the
	 * smoothed predicted-echo/residual power ratio, applied to the
	 * residual's own overlap-save frame through a short causal
	 * (D-delayed, Hann-windowed) gain kernel so the product stays
	 * alias-free.
	 */
	if (atomic_get(&sup_enabled) != 0) {
		if (!sup.kwin_ready) {
			/* Hann via the cosine recurrence (no per-tap cosf) */
			float cp = 1.0f, cc = AEC_SUP_HANN_C;

			sup.kwin[0] = 0.0f;
			sup.kwin[1] = 0.5f * (1.0f - cc);
			for (uint32_t j = 2; j < AEC_SUP_KLEN; j++) {
				float cn = 2.0f * AEC_SUP_HANN_C * cc - cp;

				cp = cc;
				cc = cn;
				sup.kwin[j] = 0.5f * (1.0f - cn);
			}
			sup.kwin_ready = true;
		}

		/* slide the residual history and append this block (pcm
		 * currently holds the emitted residual)
		 */
		memmove(sup.res_hist, sup.res_hist + FD_H,
			(FD_N - FD_H) * sizeof(float));
		for (uint32_t i = 0; i < FD_H; i++) {
			sup.res_hist[FD_N - FD_H + i] =
				(float)pcm[i] * (1.0f / 32768.0f);
		}

		/* residual power spectrum (Sy was measured on Y above) */
		memcpy(fd.buf, sup.res_hist, sizeof(fd.buf));
		aec_rfft(fd.buf, fd.spec);

		float gmin = 1.0f;

#if AEC_SUP_GCAP_ENV_GATE
		/* reference-unreliable fail-safe (see AEC_SUP_REF_UNREL_HOPS):
		 * while the window is off the written ring the residual is
		 * uncancelled structured echo, so re-arm the deep onset duck
		 * (like a resync) to scramble it below the VAD. Done before ob
		 * so the deepening applies this same block. */
		if (aec.ref_unrel_hops > 0 && sup.onset < AEC_SUP_RESYNC_HOPS) {
			sup.onset = AEC_SUP_RESYNC_HOPS;
		}
#endif

		/* onset boost weight ob: held at 1.0 for the first
		 * ONSET_HOLD_HOPS (full duck / boosted beta+floor), then eased
		 * linearly to 0 (steady) over the remaining tail. sup.onset
		 * counts down from ONSET_HOPS.
		 */
		uint32_t ease_hops = (AEC_SUP_ONSET_HOPS > AEC_SUP_ONSET_HOLD_HOPS)
				     ? (AEC_SUP_ONSET_HOPS - AEC_SUP_ONSET_HOLD_HOPS)
				     : 1;
		float ob = (sup.onset == 0) ? 0.0f
			   : (sup.onset > ease_hops)
				 ? 1.0f
				 : (float)sup.onset / (float)ease_hops;
		float beta_eff = AEC_SUP_BETA +
				 ob * (AEC_SUP_ONSET_BETA - AEC_SUP_BETA);
		float floor_eff = AEC_SUP_FLOOR +
				  ob * (AEC_SUP_ONSET_FLOOR - AEC_SUP_FLOOR);

		for (uint32_t b = FD_BIN_LO; b <= FD_BIN_HI; b++) {
			uint32_t j = b - FD_BIN_LO;
			float p = fd.spec[2 * b] * fd.spec[2 * b] +
				  fd.spec[2 * b + 1] * fd.spec[2 * b + 1];

			sup.Se[j] += AEC_SUP_POW_A * (p - sup.Se[j]);

			float graw = 1.0f - beta_eff * sup.Sy[j] /
					    (sup.Se[j] + 1e-14f);

			if (graw < floor_eff) {
				graw = floor_eff;
			}
			/* fast attack (down), slower release (up) */
			float a = (graw < sup.g[j]) ? AEC_SUP_ATT : AEC_SUP_REL;

			sup.g[j] += a * (graw - sup.g[j]);
			if (sup.g[j] < gmin) {
				gmin = sup.g[j];
			}
		}
		sup.gmin = gmin;
		if (sup.onset > 0) {
			sup.onset--;
		}

		/* zero-phase gain spectrum -> kernel; rotate to causal
		 * support [0, 2D] and Hann-taper. The truncation doubles
		 * as smoothing of the gains across bins.
		 */
		fd.spec[0] = 1.0f; /* DC and Nyquist: out of band, gain 1 */
		fd.spec[1] = 1.0f;
		/* onset blanket ceiling (see AEC_SUP_ONSET_GCAP): eased from
		 * GCAP up to 1.0 (no cap) over the onset window - a hard,
		 * prediction-independent limit on every in-band gain
		 */
		float steady_cap = AEC_SUP_STEADY_GCAP;
#if AEC_SUP_GCAP_ENV_GATE
		/* lift the sustained ceiling when either there is no real echo to
		 * scramble (idle: p_ref below the floor - fixes the dither
		 * injection) OR the envelope gate sees genuine near-end voice
		 * (barge-in). The onset window's deep cap still holds via ob.
		 * BUT never lift while the reference is unreliable (window off
		 * the written ring): there p_ref~0 falsely reads as idle and the
		 * gate's echo removal is broken, and the residual is uncancelled
		 * echo - hold the ceiling (deepened via the onset re-arm above). */
		sup.pb_hold = false;
		if (aec.ref_unrel_hops > 0) {
			/* keep steady_cap = AEC_SUP_STEADY_GCAP (fail safe) */
		} else if (sup.gate_released) {
			steady_cap = 1.0f; /* genuine near-end voice: release */
		} else if (aec.p_ref < AEC_SUP_GATE_PREF_MIN) {
			/* p_ref below the echo floor. This is genuine idle ONLY if
			 * the speaker is not actively playing: a live reference
			 * feed-stall / re-anchor (the barge-flush collapse) also
			 * reads p_ref~0 with POSITIVE margin (so REF_UNREL misses
			 * it) while real echo is present, because the aligned
			 * window momentarily serves silence though real audio is
			 * still being written to the ring. Recent non-silent
			 * playback => hold the cap (keep the echo scrambled);
			 * stale => truly idle, lift (no dither). See
			 * AEC_SUP_PLAYBACK_HOLD_MS. */
			uint32_t rf =
				(uint32_t)atomic_get(&aec.real_feed_stamp_ms);
			int32_t rf_age =
				(int32_t)((uint32_t)k_uptime_get() - rf);

			if (rf == 0 || rf_age > (int32_t)AEC_SUP_PLAYBACK_HOLD_MS) {
				steady_cap = 1.0f; /* genuinely idle */
			} else {
				sup.pb_hold = true; /* collapse guard: hold cap */
			}
		}
#elif AEC_SUP_GCAP_DTD_GATE
		if (aec.p_err >= AEC_SUP_GCAP_DTD_RATIO * aec.p_ref) {
			steady_cap = 1.0f; /* near-end: release the sustained cap */
		}
#endif
		float g_cap = AEC_SUP_ONSET_GCAP +
			      (steady_cap - AEC_SUP_ONSET_GCAP) * (1.0f - ob);
		for (uint32_t b = 1; b < FD_BINS; b++) {
			float gj = 1.0f;

			if (b >= FD_BIN_LO && b <= FD_BIN_HI) {
				gj = sup.g[b - FD_BIN_LO];
				if (gj > g_cap) {
					gj = g_cap;
				}
			} else if (b >= FD_CAP_BIN_LO && b < FD_BIN_LO) {
				/* bone-conduction echo band, below adaptation:
				 * blanket cap only (no per-bin prediction here) */
				gj = g_cap;
			}
			fd.spec[2 * b] = gj;
			fd.spec[2 * b + 1] = 0.0f;
		}
		aec_irfft(fd.spec, fd.buf);
		for (uint32_t j = 0; j < AEC_SUP_KLEN; j++) {
			uint32_t src = (j < AEC_SUP_D)
				       ? FD_N - AEC_SUP_D + j
				       : j - AEC_SUP_D;

			sup.kc[j] = fd.buf[src] * sup.kwin[j];
		}

		/* apply by direct convolution over the residual history
		 * (KLEN taps x FD_H samples ~ 82k MACs): output = the
		 * gain-filtered residual, content delayed by D samples
		 */
		for (uint32_t i = 0; i < FD_H; i++) {
			const float *r = sup.res_hist + FD_N - FD_H + i;
			float acc = 0.0f;

			for (uint32_t j = 0; j < AEC_SUP_KLEN; j++) {
				acc += sup.kc[j] * r[-(int32_t)j];
			}

			float out = acc * 32768.0f;

			if (out > 32767.0f) {
				out = 32767.0f;
			} else if (out < -32768.0f) {
				out = -32768.0f;
			}
			pcm[i] = (int16_t)out;
		}
	}

	fd.frame++;
	return adapt;
}
#endif /* CONFIG_HALO_AUDIO_AEC_FDAF */

void audio_aec_note_mic_loss(size_t samples)
{
	if (samples == 0) {
		return;
	}

	/* the lost samples occupied real capture time: advancing the
	 * cumulative count keeps every later block's reconstructed capture
	 * time honest. The reference window jump this causes is truth (the
	 * echo really is that much further on); a jump past the drift
	 * tolerance re-anchors through the normal resync path. The mic
	 * history now splices across the hole, so hold adaptation briefly
	 * exactly like a history-wipe edge - prediction and subtraction
	 * stay on.
	 */
	aec.mic_total += samples;
	aec.mic_lost += samples;
#if defined(CONFIG_HALO_AUDIO_AEC_FDAF)
	aec.fd_hist_fill = 0;
#else
	aec.mu_ramp = 0;
#endif
}

void audio_aec_process(int16_t *pcm, size_t samples, uint32_t sample_rate,
		       uint8_t channels)
{
	size_t mono = (channels > 1) ? samples / channels : samples;

	/* rate probe counts every capture block, independent of AEC state */
	clkmon_note(&aec.clkmon_mic, mono);

	/* capture-epoch estimate for the physical-time ref pairing; runs
	 * regardless of AEC state so A/B toggles don't restart the timeline
	 */
	int64_t now_ms = k_uptime_get();

	if (aec.last_mic_ms == 0 || (now_ms - aec.last_mic_ms) > 200) {
		/* fresh mic session: restart the capture timeline */
		aec.mic_total = 0;
		aec.cap_epoch_ms = INT64_MAX;
		aec.synced = false;
		have_held = false;
		aec.late_bucket_ms = 0;
	}
	aec.last_mic_ms = now_ms;
	aec.mic_total += mono;

	int64_t c = now_ms - (int64_t)(aec.mic_total / 16u);

	epoch_observe(&aec.cap_epoch_ms, &aec.cap_epoch_set_ms, c, now_ms,
		      &aec.cap_refines, &aec.cap_refine_ms,
		      &aec.cap_late, &aec.cap_late_ms);

	/* late-floor backstop (see AEC_LATE_FLOOR_MS) */
	if (aec.late_bucket_ms == 0) {
		aec.late_bucket_ms = now_ms;
		aec.late_floor_cur = INT32_MAX;
		aec.late_floor_prev = INT32_MAX;
	}

	int32_t late = (int32_t)(c - aec.cap_epoch_ms);

	if (late < aec.late_floor_cur) {
		aec.late_floor_cur = late;
	}
	if ((now_ms - aec.late_bucket_ms) >= AEC_LATE_FLOOR_WIN_MS) {
		int32_t floor = MIN(aec.late_floor_cur, aec.late_floor_prev);

		if (floor != INT32_MAX && floor > AEC_LATE_FLOOR_MS) {
			/* slide by less than the observed floor: the floor
			 * includes the min-backlog observation noise, and
			 * sliding by all of it bakes that noise in every
			 * time (measured as a slow negative margin drift).
			 * Under-correction is self-healing - the remainder
			 * stays in the next window's floor.
			 */
			int32_t slide = floor - AEC_LATE_FLOOR_MS / 2;

			aec.cap_epoch_ms += slide;
			aec.cap_epoch_set_ms = now_ms;
			aec.cap_slips++;
			aec.cap_slip_ms += (uint32_t)slide;
			LOG_INF("AEC cap epoch slid +%dms (unreported "
				"capture loss), slip #%u",
				slide, aec.cap_slips);
			aec.late_floor_prev = INT32_MAX;
#ifndef AEC_LATE_FLOOR_FWD_ONLY
		} else if (floor != INT32_MAX && floor < -AEC_LATE_FLOOR_MS) {
			/* Mirror of the forward slide, for a too-LATE anchor:
			 * the floor is persistently EARLIER than the epoch, so
			 * the anchor sits later than the real capture timeline.
			 * A transient stall over-slid it forward and the flush
			 * that ended the stall landed after the freeze reclosed,
			 * so epoch_observe (down-refine, freeze-bounded) could
			 * not pull it back. Left alone cap_end_ms runs ahead of
			 * real time, desired_end walks past the write head, and
			 * margin latches negative - the read window never
			 * re-anchors and the near-end crushes to silence. The
			 * forward slide above only chases loss (later), never
			 * this direction, so slide the epoch EARLIER by the
			 * persistent over-lateness (same half-floor under-
			 * correction against burst-flush jitter) to pull the
			 * window back onto the ring. Healthy runs keep floor>=0
			 * (the epoch IS the min), so this only ever undoes an
			 * over-slide - it cannot misfire.
			 */
			int32_t slide = floor + AEC_LATE_FLOOR_MS / 2;

			aec.cap_epoch_ms += slide;
			aec.cap_epoch_set_ms = now_ms;
			aec.cap_slips++;
			aec.cap_slip_ms += (uint32_t)(-slide);
			LOG_INF("AEC cap epoch slid %dms (anchor too late), "
				"slip #%u",
				slide, aec.cap_slips);
			aec.late_floor_prev = INT32_MAX;
#endif /* !AEC_LATE_FLOOR_FWD_ONLY */
		} else {
			aec.late_floor_prev = aec.late_floor_cur;
		}
		aec.late_floor_cur = INT32_MAX;
		aec.late_bucket_ms = now_ms;
	}

	if (atomic_get(&aec.enabled) == 0 || !aec.ref_format_ok) {
		return;
	}

	if (sample_rate != AEC_SAMPLE_RATE || channels != 1) {
		if (!aec.warned_mic_format) {
			aec.warned_mic_format = true;
			LOG_WRN("AEC bypassed: mic session %u Hz %u ch "
				"(need %u Hz mono)",
				sample_rate, channels, AEC_SAMPLE_RATE);
		}
		return;
	}
	aec.warned_mic_format = false;

	/* speaker-idle fast path (see AEC_SPK_HANGOVER_MS): with no recent
	 * reference feed there is no echo to cancel - pass through with
	 * zero added latency and no filtering work. The held block, if any,
	 * is a hangover old by now: drop it rather than splice stale audio.
	 */
	uint32_t feed_ms = (uint32_t)atomic_get(&aec.feed_stamp_ms);
	/* signed age: the tap ISR can stamp feed_ms between this function
	 * reading now_ms and reaching here, making the stamp newer than
	 * now_ms - the unsigned difference then wraps huge and fakes a
	 * disengage (held block dropped, sync lost, FD history wiped, all
	 * without a resync count). A negative age means "just fed".
	 */
	int32_t feed_age = (int32_t)((uint32_t)now_ms - feed_ms);

	if (feed_ms == 0 || feed_age > (int32_t)AEC_SPK_HANGOVER_MS) {
		have_held = false;
		aec.synced = false;
		return;
	}

	/* one-block hold-back (see AEC_REF_LEAD): swap the arriving block
	 * with the held one and cancel the held content against the
	 * arrival's (one block newer) window. First block of a session
	 * emits silence.
	 */
	if (samples == AEC_MAX_BLOCK) {
		for (size_t i = 0; i < AEC_MAX_BLOCK; i++) {
			int16_t t = pcm[i];

			pcm[i] = held_blk[i];
			held_blk[i] = t;
		}
		if (!have_held) {
			memset(pcm, 0, AEC_MAX_BLOCK * sizeof(int16_t));
			have_held = true;
#if defined(CONFIG_HALO_AUDIO_AEC_FDAF)
			/* fresh engage: the suppressor's residual history
			 * still holds the previous playback's tail (>= a
			 * hangover old) - clear it so the first output
			 * frames cannot replay it
			 */
			memset(sup.res_hist, 0, sizeof(sup.res_hist));
			/* onset boost is armed by the reference gate's rising
			 * edge (below), not here: the silence-feed keeps the
			 * session engaged so this engage transition fires only
			 * once per session, not once per reply
			 */
#endif
			return;
		}
	}

	uint32_t t_start = k_cyc_to_us_floor32(k_cycle_get_32());

	while (samples > 0) {
		size_t n = MIN(samples, (size_t)AEC_MAX_BLOCK);

		/* slide the reference histories and append this block's worth */
		memmove(aec.x, aec.x + n, (AEC_TAPS + AEC_MAX_BLOCK - n) * sizeof(float));
		memmove(aec.xf, aec.xf + n, (AEC_TAPS + AEC_MAX_BLOCK - n) * sizeof(float));
		float *x_new = aec.x + AEC_TAPS + AEC_MAX_BLOCK - n;
		float *xf_new = aec.xf + AEC_TAPS + AEC_MAX_BLOCK - n;

		size_t take = ref_consume(x_new, xf_new, n);

		/* adaptation gate: only learn while the reference actually
		 * carries signal; a silent/dithered reference (idle or paused
		 * speaker session) has nothing to learn from and the filter
		 * would slowly diverge trying to explain mic noise with it
		 */
		float ref_e2 = 0.0f;

		for (size_t i = 0; i < take; i++) {
			ref_e2 += x_new[i] * x_new[i];
		}
		if (take > 0 &&
		    ref_e2 > AEC_REF_GATE_RMS * AEC_REF_GATE_RMS * (float)take) {
#if defined(CONFIG_HALO_AUDIO_AEC_FDAF)
			/* reference silence->active = a reply onset. THIS is the
			 * per-reply onset trigger: the round-8 driver silence-
			 * feed keeps the speaker session continuously engaged
			 * across reply gaps, so the speaker-idle re-engage never
			 * fires between replies - only the reference gate does.
			 * Head-worn, every reply's onset residual trips the
			 * server VAD before the steady suppressor re-establishes,
			 * so re-arm the onset boost on the gate's rising edge.
			 */
			if (aec.gate_hangover == 0) {
				sup.onset = AEC_SUP_ONSET_HOPS;
			}
#endif
			aec.gate_hangover = AEC_GATE_HANGOVER_BLOCKS;
		} else if (aec.gate_hangover > 0) {
			/* gate closing pauses adaptation but does NOT reset
			 * the mu ramp: a converged filter resuming after a
			 * speech pause needs no soft-start (utterance-gappy
			 * speech otherwise spends a third of its time at
			 * crippled mu), only a fresh or re-anchored one does
			 * (resync and enable reset the ramp)
			 */
			aec.gate_hangover--;
		}

		bool adapt_ok = aec.gate_hangover > 0;
		bool adapted = false;

		/* one-shot paired capture: the raw mic block and the raw
		 * reference window it is about to be cancelled against.
		 * Only a fully-served window during active playback is
		 * worth correlating.
		 */
		if (atomic_get(&pair_req) != 0 && aec.synced &&
		    take == n && aec.gate_hangover > 0) {
			memcpy(pair_mic, pcm, n * sizeof(int16_t));
			for (size_t k = 0; k < AEC_TAPS + n; k++) {
				/* x holds int16/32768 exactly */
				pair_ref[k] = (int16_t)(aec.x[(AEC_MAX_BLOCK - n) + k] *
							32768.0f);
			}
			pair_n = n;
			atomic_set(&pair_req, 0);
			atomic_set(&pair_done, 1);
		}

#if defined(CONFIG_HALO_AUDIO_AEC_FDAF)
		if (n == FD_H) {
			adapted = fd_process_block(pcm, adapt_ok);
		}
		/* a non-hop-sized chunk passes through untouched - the mic
		 * path always delivers whole 20ms blocks (and the hold-back
		 * only engages on them)
		 */
		(void)adapted;
#else
		/* recompute the update-vector norm at each block start so the
		 * cheap per-sample incremental update can't drift
		 */
		{
			const float *xfv0 = aec.xf + (AEC_MAX_BLOCK - n);
			float fnorm2 = 0.0f;

			for (size_t k = 0; k < AEC_TAPS; k++) {
				fnorm2 += xfv0[k] * xfv0[k];
			}
			aec.xf_norm2 = fnorm2;
		}

		/* proportionate step gains for this block (see
		 * AEC_PNLMS_ALPHA): mean-1-normalized blend of uniform and
		 * |w|-proportionate
		 */
		if (adapt_ok) {
			float wsum = 0.0f;

			for (size_t k = 0; k < AEC_TAPS; k++) {
				wsum += __builtin_fabsf(aec.w[k]);
			}

			float uni = 1.0f - AEC_PNLMS_ALPHA;
			float prop = AEC_PNLMS_ALPHA * (float)AEC_TAPS /
				     (wsum + 1e-6f);

			for (size_t k = 0; k < AEC_TAPS; k++) {
				float gk = uni + prop * __builtin_fabsf(aec.w[k]);

				/* stability under the global (non-G-weighted)
				 * normalization: bound the concentration
				 */
				aec.wgain[k] = MIN(gk, 16.0f);
			}
		}

		for (size_t i = 0; i < n; i++) {
			/* reference vector for output sample i ends at the
			 * sample aligned with it (index AEC_MAX_BLOCK - n + i
			 * offsets within the sliding window)
			 */
			const float *xv = aec.x + (AEC_MAX_BLOCK - n) + i;
			const float *xfv = aec.xf + (AEC_MAX_BLOCK - n) + i;

			if (i > 0) {
				/* window slid one sample: update the norm of
				 * the (high-passed) update vector
				 */
				float enter = xfv[AEC_TAPS - 1];
				float leave = xfv[-1];

				aec.xf_norm2 += enter * enter - leave * leave;
				if (aec.xf_norm2 < 0.0f) {
					aec.xf_norm2 = 0.0f;
				}
			}

			float y = 0.0f;

			for (size_t k = 0; k < AEC_TAPS; k++) {
				y += aec.w[k] * xv[k];
			}

			float d = (float)pcm[i] * (1.0f / 32768.0f);

			/* high-passed view of the mic for all internal
			 * measures (see AEC_HPF_ALPHA)
			 */
			aec.mic_lf1 += AEC_HPF_ALPHA * (d - aec.mic_lf1);

			float d_hp1 = d - aec.mic_lf1;

			aec.mic_lf2 += AEC_HPF_ALPHA * (d_hp1 - aec.mic_lf2);

			float d_hp = d_hp1 - aec.mic_lf2;

			/* echo-estimate sanity clamp: the echo cannot be
			 * louder than the (rumble-free) mic signal containing
			 * it, so an estimate far above that level is a
			 * misadapted filter - bound it so no transient can
			 * become an audible burst
			 */
			aec.p_mic += AEC_POW_ALPHA * (d_hp * d_hp - aec.p_mic);
			if (y * y > AEC_Y_CLAMP_P * aec.p_mic) {
				float lim = __builtin_sqrtf(AEC_Y_CLAMP_P * aec.p_mic);

				y = (y > 0.0f) ? lim : -lim;
			}

			/* audio output: subtract from the untouched mic */
			float e = d - y;

			/* filtered error for the update and power measures:
			 * same high-pass as the xf history, so the update's
			 * fixed point is the true echo path in-band
			 */
			aec.err_lf1 += AEC_HPF_ALPHA * (e - aec.err_lf1);

			float e_f1 = e - aec.err_lf1;

			aec.err_lf2 += AEC_HPF_ALPHA * (e_f1 - aec.err_lf2);

			float e_f = e_f1 - aec.err_lf2;

			/* matching low-pass + pre-emphasis (see AEC_LPF_ALPHA,
			 * AEC_PREEMPH)
			 */
			for (int s = 0; s < 3; s++) {
				aec.err_lp[s] += AEC_LPF_ALPHA * (e_f - aec.err_lp[s]);
				e_f = aec.err_lp[s];
			}

			float e_pe = e_f - AEC_PREEMPH * aec.err_pe;

			aec.err_pe = e_f;
			e_f = e_pe;

			float out = e * 32768.0f;

			if (out > 32767.0f) {
				out = 32767.0f;
			} else if (out < -32768.0f) {
				out = -32768.0f;
			}
			pcm[i] = (int16_t)out;

			float xn = xfv[AEC_TAPS - 1];

			aec.p_ref += AEC_POW_ALPHA * (xn * xn - aec.p_ref);
			aec.p_err += AEC_POW_ALPHA * (e_f * e_f - aec.p_err);

			/* adapt only when the error is explainable as echo:
			 * large error relative to reference power means the
			 * wearer is speaking (double-talk) - freeze
			 */
			if (adapt_ok && aec.p_ref > 1e-8f &&
			    aec.p_err < AEC_DTD_THRESHOLD * aec.p_ref) {
				float mu = AEC_MU;

				/* soft-start after each gate-on */
				if (aec.mu_ramp < AEC_MU_RAMP_SAMPLES) {
					aec.mu_ramp++;
					mu = AEC_MU * (float)aec.mu_ramp /
					     (float)AEC_MU_RAMP_SAMPLES;
				}

				float g = mu * e_f / (aec.xf_norm2 + AEC_EPS);

				for (size_t k = 0; k < AEC_TAPS; k++) {
					aec.w[k] += g * aec.wgain[k] * xfv[k];
				}
				adapted = true;
			}
		}

		/* divergence guard: rescale to WELL BELOW the threshold so a
		 * diverging filter is knocked back instead of pinned at the
		 * clamp, but a legitimately strong echo path is untouched.
		 * (Fused with the per-block leak - see AEC_W_LEAK.)
		 */
		float wn = 0.0f;
		float leak = adapted ? AEC_W_LEAK : 1.0f;

		for (size_t k = 0; k < AEC_TAPS; k++) {
			aec.w[k] *= leak;
			wn += aec.w[k] * aec.w[k];
		}
		if (wn > AEC_NORM_CLAMP) {
			float s = __builtin_sqrtf((AEC_NORM_CLAMP / 4.0f) / wn);

			for (size_t k = 0; k < AEC_TAPS; k++) {
				aec.w[k] *= s;
			}
			if ((aec.norm_clamps++ % 100) == 0) {
				LOG_WRN("AEC norm clamp #%u (%.1f)",
					aec.norm_clamps, (double)wn);
			}
		}
#endif /* CONFIG_HALO_AUDIO_AEC_FDAF */

		pcm += n;
		samples -= n;
	}

	/* load accounting: running average over ~10s, and an overload
	 * bypass so a too-slow filter can't starve the mic pipeline
	 */
	uint32_t block_us = k_cyc_to_us_floor32(k_cycle_get_32()) - t_start;

	aec.load_us_accum += block_us;
	if (++aec.load_blocks >= 500) {
		LOG_INF("AEC load: avg %u us per block (budget %u)",
			aec.load_us_accum / aec.load_blocks, AEC_BLOCK_BUDGET_US);
		aec.load_us_accum = 0;
		aec.load_blocks = 0;
	}
	if (block_us > AEC_BLOCK_BUDGET_US) {
		if (++aec.overload_streak >= AEC_OVERLOAD_BLOCKS) {
			LOG_ERR("AEC overloaded (%u us/block) - bypassing", block_us);
			audio_aec_enable(false);
		}
	} else {
		aec.overload_streak = 0;
	}
}

const float *audio_aec_snapshot(struct audio_aec_stats *stats, size_t *taps)
{
	float wn = 0.0f;

	for (size_t k = 0; k < AEC_TAPS; k++) {
		wn += aec.w[k] * aec.w[k];
	}
	stats->w_norm2 = wn;
#if defined(CONFIG_HALO_AUDIO_AEC_FDAF)
	stats->sup_gmin = sup.gmin;
	/* Live frequency-domain filter energy (sum|W|^2 over every
	 * partition), so convergence is observable immediately instead of
	 * waiting for the round-robin shadow taps that back w_norm2. Only
	 * runs at snapshot cadence, so the FD_K*FD_N sweep is cheap.
	 */
	{
		float fdwn = 0.0f;
		for (size_t k = 0; k < FD_K; k++) {
			for (size_t i = 0; i < FD_N; i++) {
				fdwn += fd.W[k][i] * fd.W[k][i];
			}
		}
		stats->fd_wnorm2 = fdwn;
	}
	{
		float sy = 0.0f, se = 0.0f, gsum = 0.0f;
		for (size_t j = 0; j < AEC_SUP_NBINS; j++) {
			sy += sup.Sy[j];
			se += sup.Se[j];
			gsum += sup.g[j];
		}
		stats->sup_sy = sy;
		stats->sup_se = se;
		stats->sup_gmean = gsum / (float)AEC_SUP_NBINS;
	}
	stats->sup_onset = sup.onset;
#if AEC_SUP_GCAP_ENV_GATE
	stats->sup_gate_fast = sup.gate_fast;
	stats->sup_gate_floor = sup.gate_floor;
	stats->sup_gate_mid = sup.gate_mid;
	stats->sup_gate_rel = sup.gate_released ? 1u : 0u;
	stats->sup_pb_hold = sup.pb_hold ? 1u : 0u;
#else
	stats->sup_gate_fast = 0.0f;
	stats->sup_gate_floor = 0.0f;
	stats->sup_gate_mid = 0.0f;
	stats->sup_gate_rel = 0u;
	stats->sup_pb_hold = 0u;
#endif
#else
	stats->sup_gmin = 1.0f;
	stats->fd_wnorm2 = 0.0f;
	stats->sup_sy = 0.0f;
	stats->sup_se = 0.0f;
	stats->sup_gmean = 1.0f;
	stats->sup_onset = 0;
	stats->sup_gate_fast = 0.0f;
	stats->sup_gate_floor = 0.0f;
	stats->sup_gate_mid = 0.0f;
	stats->sup_gate_rel = 0u;
	stats->sup_pb_hold = 0u;
#endif
	stats->p_ref = aec.p_ref;
	stats->p_err = aec.p_err;
	stats->p_mic = aec.p_mic;
	stats->resyncs = aec.resyncs;
	stats->norm_clamps = aec.norm_clamps;
	stats->ref_underruns = aec.ref_underruns;
	stats->ref_pads = aec.ref_pads;
	stats->feed_gaps = aec.feed_gaps;
	stats->feed_gap_ms = aec.feed_gap_ms;
	stats->emit_refines = aec.emit_refines;
	stats->emit_refine_ms = aec.emit_refine_ms;
	stats->cap_refines = aec.cap_refines;
	stats->cap_refine_ms = aec.cap_refine_ms;
	stats->emit_late = aec.emit_late;
	stats->emit_late_ms = aec.emit_late_ms;
	stats->cap_late = aec.cap_late;
	stats->cap_late_ms = aec.cap_late_ms;
	stats->mic_lost = aec.mic_lost;
	stats->cap_slips = aec.cap_slips;
	stats->cap_slip_ms = aec.cap_slip_ms;
	stats->cap_late_floor =
		(aec.late_bucket_ms != 0 &&
		 MIN(aec.late_floor_cur, aec.late_floor_prev) != INT32_MAX)
			? MIN(aec.late_floor_cur, aec.late_floor_prev)
			: 0;
	stats->emit_slips = aec.emit_slips;
	stats->emit_slip_ms = aec.emit_slip_ms;
	stats->margin_last = aec.margin_last;
	stats->margin_min = aec.margin_min;
	stats->margin_max = aec.margin_max;
	stats->ref_skew_adj = aec.ref_skew_adj;
	stats->mic_frames = aec.clkmon_mic.frames;
	stats->mic_us = aec.clkmon_mic.us;
	stats->mic_runs = aec.clkmon_mic.runs;
	stats->ref_frames = aec.clkmon_ref.frames;
	stats->ref_us = aec.clkmon_ref.us;
	stats->ref_runs = aec.clkmon_ref.runs;
	*taps = AEC_TAPS;

	return aec.w;
}

void audio_aec_clkmon_zero(void)
{
	memset(&aec.clkmon_mic, 0, sizeof(aec.clkmon_mic));
	memset(&aec.clkmon_ref, 0, sizeof(aec.clkmon_ref));
}

void audio_aec_pair_request(void)
{
	atomic_set(&pair_done, 0);
	atomic_set(&pair_req, 1);
}

size_t audio_aec_pair_read(const int16_t **mic, const int16_t **ref,
			   size_t *taps)
{
	if (atomic_get(&pair_done) == 0) {
		return 0;
	}
	*mic = pair_mic;
	*ref = pair_ref;
	*taps = AEC_TAPS;
	return pair_n;
}

/* Register the reference tap once the driver layer is up */
static int audio_aec_sys_init(void)
{
	max98357a_audio_set_tx_tap(audio_aec_feed_reference);
#if defined(CONFIG_HALO_AUDIO_AEC_FDAF)
	/* the ITCM section is NOLOAD - not zeroed by kernel startup; the
	 * suppressor gains rest at 1 (passthrough), not 0
	 */
	memset(&fd, 0, sizeof(fd));
	memset(&sup, 0, sizeof(sup));
	for (size_t j = 0; j < AEC_SUP_NBINS; j++) {
		sup.g[j] = 1.0f;
	}
	sup.gmin = 1.0f;
	aec_fft_init();
	LOG_INF("AEC initialized: FDAF %dx%d taps (%d ms) at %d Hz",
		FD_K, FD_H, FD_K * FD_H * 1000 / AEC_SAMPLE_RATE,
		AEC_SAMPLE_RATE);
#else
	LOG_INF("AEC initialized: %d taps (%d ms) at %d Hz",
		AEC_TAPS, AEC_TAPS * 1000 / AEC_SAMPLE_RATE, AEC_SAMPLE_RATE);
#endif
	return 0;
}

SYS_INIT(audio_aec_sys_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
