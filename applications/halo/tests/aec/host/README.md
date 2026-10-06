# Host-side AEC validation

Compiles `modules/halo/src/audio_aec.c` against stub Zephyr headers and runs
it through the failure modes measured on the Halo unit (2026-07-12): onset
divergence burst, idle-dither injection, pause/resume, periodic reference
underruns, baseline convergence with a device-realistic mic rumble
(~-26dBFS sub-100Hz, 14dB above the speech-band echo) and H2-style
distortion, plus the anchor-churn mode: a post-convergence scheduling-luck
epoch refinement, which unfrozen causes a resync (history wipe + shifted
re-anchor, ERLE collapse) and frozen must be counted and suppressed. Check 7
validates the paired capture (`aec('pair')`) against the known synthetic
echo delay via a matched filter; check 10 the speaker-idle bypass
(bit-exact zero-latency passthrough while the speaker is silent, clean
re-engage on resume).

Checks 11-13 cover the barge-in onset stage (2026-07-12): 11 plays the
speech-like stimulus into a COLD filter and scores ERLE over the first
1.5s (the "first assistant reply" gap that false-triggered
interruptions); 12 re-engages a WARM filter after a speaker-idle gap
(every later reply) and requires immediate cancellation; 13 injects
near-end speech ~5dB above the echo during playback and bounds the
median output attenuation of near-active blocks - the residual
suppressor must not eat the wearer's barge-in. Scoring detail: the
FDAF suppressor delays output content by its gain-kernel group delay
(SUP_D = 128 samples), so the known-noise exclusion is shifted to
match, burst checks compare against the louder of the two adjacent
input blocks (a falling envelope otherwise reads its own slope as fake
excess), and the first 3 blocks of each engage transition are exempt
(the inserted hold-back silence and the starting delay stream are
mis-scored by construction; real divergence bursts run hundreds of ms).

Check 8 adds second-differenced white noise (+12dB/oct, mostly above
4kHz) to the mic and scores in-band ERLE with the known noise excluded. It
guards the linear filter's band-limited update under HF noise (the TD
build's `AEC_LPF_ALPHA`). In the FDAF build it runs with the residual
suppressor made transparent through the tune API (`sup_beta` 0,
`sup_floor` 1, every cap 1; restored after), so it scores the linear
stage: 14.2dB, limit 10. With the suppressor on, its per-block kernel
scales the known noise too, so the exclusion no longer matched what is in
the output and the score mostly measured how hard the playback ceiling
attenuates noise that is not echo: 5.25dB at `steady_gcap` 0.25 against
21.6dB of echo-only residual, falling as the cap deepened while the
echo-only residual improved, and `AEC_FD_MU=1.0f` (linear stage 14.2 ->
8.5dB) still passed it at 5.1. (`audio_aec_suppress(false)` is no
substitute: it drops the kernel's SUP_D delay the scoring expects.)

Check 15 is the capture-loss case (2026-07-13, the live-duplex death at
~100s): the reference window is anchored by cumulative SEEN mic samples
- and so are both operands of the drift check - so mic blocks the PDM
layer loses under load (FIFO overflow clears, queue-full drops) walk the
window off the reference ring at the loss rate with zero resyncs counted
(Halo unit: margin_max 7019, monotone, ring all-pad, filter and suppressor
both dead). Part (a) drops one mic block every 2s, REPORTED via
audio_aec_note_mic_loss (the driver's drop ledger): each loss must
re-anchor as a counted resync, per-cycle ERLE must hold, margin must stay
at baseline. Part (b) drops a 5-block burst UNREPORTED: the late-floor
backstop (windowed-min lateness of the capture-epoch observations) must
slide the epoch, re-anchor, and recover cancellation within seconds.
Part (c) is the mirror direction (worn Halo unit session 212454, 2026-07-15,
the feat/aec landing blocker): after the freeze has closed, a backlog
FLUSH (mic_total jumps in a burst, the drained samples arriving late)
pushes the capture-epoch observation persistently EARLIER than the frozen
anchor. epoch_observe only refines earlier inside the freeze and the
forward slide only moves later, so a forward-only backstop leaves the
epoch stuck too late - cap_end runs ahead of real time, the window walks
PAST the write head, margin latches negative, cap_late_ms climbs
unbounded and the near-end crushes to silence with zero cap_slips. The
symmetric backstop must slide the epoch earlier and re-anchor within one
window; the check asserts the walk-off occurred (worst margin < -400), a
backward slip fired, and cap_late_ms stayed bounded (ERLE is not a valid
discriminator here - the FDAF suppressor ducks so hard on the latched
REF_UNREL that the pre-fix build posts a HIGHER ERLE while crushing the
near-end).

Check 14 is the live BLE-duplex case (2026-07-12): eight reply cycles
(reply gap / speech span / re-engage) per timing recipe - identical
timing, per-reply tap-callback latency, per-reply consumer latency, a
60ms mid-reply starve, and all combined - scoring in-band ERLE per
reply. Gaps are zero-fed, modelling the MAX98357A driver's
silence-feed (on queue drain the completion callback clocks zero
blocks while the session is open, so the reference timeline never
breaks and the emission epoch is established once per session). All
recipes must hold cancellation in both builds.

Check 17 is the stale-session case (bug A, 2026-10-04): a short earcon
arms the onset duck, then the speaker session closes (speaker-idle
bypass) or the AEC is re-enabled before the duck has run out. The earcon
follows a 1.2s silence-fed gap (longer than the 1s re-arm hold-off), so
it is a new reply, and the check requires that it armed the duck
(`earcon duck` > 0). The next
session opens with silence feed (`speaker.start`, nothing queued) while a
second near-end talker speaks. There is no echo, so no fully-voiced near
block may be attenuated by more than 10dB. Before the fix the last
hangover blocks before the bypass always armed the ref-unreliable
fail-safe (the read window walks past the frozen write head), the bypass
froze it with `onset` at 49, and the next session muted the wearer by
~34dB for ~1.0-1.4s (36/59 and 38/60 blocks crushed). Two INFO lines
follow: 18 reports whether the first reply after a session close still
arms its onset duck (it must: the bypass clears the adaptation hangover
so the rising edge re-arms), 21 counts envelope-gate releases on an
echo-only cold onset (released blocks there are false releases; the
gate is held while the reference history refills after a wipe).
`DUMP12=1` / `DUMP17=1` print per-block traces of checks 12 and 17.

Checks 19, 20 and 22 cover the onset duck's re-arm (2026-10-04). The
duck used to re-arm on every reference rising edge after the adaptation
gate's 160ms hangover, so every pause inside a reply re-ducked the wearer
by 34dB for ~0.4s; it now re-arms only after AEC_SUP_ONSET_REARM_MS
(1000ms) of reference silence, and a near-end gate release lifts its
blanket ceiling once the filter is warm (AEC_SUP_ONSET_GATE_LIFT). 19
runs a warm 10s speech reply (1.5s talk / 0.5s pause) with the wearer
talking over all of it: zero re-arms and fewer than 1 in 20 near blocks
cut by more than 15dB (before: 6 re-arms, 95/370). 20 is the guard on the
other side: a new reply after a 1.5s silence-fed gap must still arm the
duck and stay cancelled (ERLE(0-1s) > 12dB). 22 has the wearer talk
through a 1.2s reply gap and into the next reply, scored over the duck
window: fewer than 1 in 10 near blocks cut by more than 15dB (before:
11/25, median -13.5dB). With the item-5 gate (KAPPA 0.15, full band)
19's median out-in sat near -10dB: outside the duck the wearer was held
under the -12dB steady cap on most blocks. With the 2026-10-04 defaults
(KAPPA 0.5, gate band below 1kHz, ABSFLOOR 0.9, HANG 60, BETA 1.25,
FLOOR 0.15, two-band ceiling 750Hz / 0.5) it is -2.0dB (p10 -4.1, 5/370
cut by more than 15dB); the top cap band does not change it.

The gate band (`AEC_SUP_GATE_BAND_HZ`), the two-band ceiling
(`AEC_SUP_CAP_SPLIT_HZ`, `AEC_SUP_CAP_LO_GCAP`) and the top cap band
(`AEC_SUP_CAP_HI_SPLIT_HZ`, `AEC_SUP_CAP_HI_GCAP`) were tuned offline by
replaying AEC-off device captures through this file, then A/B'd on two
units on the desk. No host check targets them directly: the synthetic echo
here has no coupling-dependent residual above 1kHz, so check 19 and the
double-talk check are where they show (19 above; 13 is -1.3dB vs -1.6).
Setting the three splits to 0 together with KAPPA 0.15, ABSFLOOR 0.5,
HANG 50, BETA 1.5 and FLOOR 0.1 restores the item-5 output byte for byte
(tune_equiv.sh), and `cap_hi_split_hz` 0 alone the output before the top
band. The top band (default 1.6kHz / 0.1) changes the host output only
slightly (the synthetic echo path rolls off above ~2kHz, so little
residual is left up there for it to cap): conv ERLE 20.0 -> 20.1dB.

Check 23 covers runtime tuning (`frame.microphone.aec_tune`, 2026-10-04),
through the same C API the Lua binding calls (`audio_aec_tune_set/get/
defaults`): the defaults equal the compile-time constants (mirrored in
test_aec.c with the same `-D` hooks, so it holds under overrides too) and
the key table covers the whole struct; out-of-range, NaN, hold > onset,
`rearm_ms` 0, `onset_gate_lift` 3, a `gate_band_hz` under 500 Hz,
`cap_split_hz` 9000, `cap_lo_gcap` 1.5, `cap_hi_split_hz` 9000,
`cap_hi_gcap` 1.5, a `cap_hi_split_hz` at or below a non-zero
`cap_split_hz`, a `gate_hf_hz` of 7985 (no bin left below Nyquist) or under
a non-zero `gate_band_hz`, and NaN, +inf and -inf in every float key, are rejected
with nothing applied; every float key accepts its min and its max (the
ranges are inclusive; among them `fd_mu` 0.05 and `gate_fast_a`/
`gate_mid_a` 0.001, which the Lua binding once rejected by comparing a
float bound widened to double), and `cap_hi_split_hz` is accepted at any
value with `cap_split_hz` 0, just above it, and off with the split at
8000; `gate_hf_hz` at 7984, at the gate band's top, and under it with the
full-band gate; and a live change takes effect
from the next block (check 19's scenario re-arms 5 times at `rearm_ms` 160
and 0 times after `'defaults'`). `rearm_ms` raised from 1000 to 5000
after enable, or after a speaker close, must still duck the first reply
(enable and the bypass preset the re-arm count to full, not to the
hold-off of that moment). `onset_ms` 0 must turn off only the reply-onset
duck: a new reply after a long gap is not ducked, and the fail-safe duck
after a backlog flush (check 16c's walk-off) still ducks with its fixed
shape: with a talker, the gate lift off and `steady_gcap` 1 (so the duck
eases toward no ceiling whatever the gate does), the talker's out-in is ~-16dB
over the held first half of that duck and rises ~9dB over the eased second
half (it stayed flat when that duck borrowed the onset ease and `onset_ms`
0 held it at full depth for the whole second).

Check 24 covers the mic gain scale (`audio_aec_set_mic_gain_scale`,
2026-10-06), which the mic stream publishes wherever it writes the PDM
gain: f = the effective mic gain over gain 1. Every near-end gate threshold
(`gate_kappa`, `gate_kappa_hf`, `gate_absfloor`, `gate_edge_abs`, gain-1
values) scales by f, and the mic-vs-reference power thresholds by f^2: the
adaptation-freeze double-talk threshold (tracked, its start value and its
bounds), the divergence guard on the filter energy (the filter is a mic /
reference ratio) and the dead-end option A release. So a run at x2.5 with
the scale must match x1 at the same set in every decision. Each case runs
one scenario from the same state (forked children): x1, the mic x2.5 in
float (gain 4, no clipping) with the scale set to 2.5, and x2.5 without it.
(a) A hot unit (echo x20 on a x0.3 mic) through a warm-up reply, an
echo-only reply and the wearer over a reply: with the scale every gate
decision matches x1 and the output matches it x2.5 to -64 dB (int16
rounding); without it 585/900 decisions differ and the gate is open on
300/300 echo-only blocks (`gate_kappa` then acts as kappa / 2.5; 0 at x1).
(b) A cold start on a stronger-coupled unit (echo x55 on a x0.15 mic), echo
only: x1 converges to 14.5 dB and x2.5 with the scale to 14.6 with every
decision equal to x1; without the scale the echo-only p_err stays above the
threshold ceiling (2 p_ref at gain-1 values), adaptation freezes and
nothing is removed (0.0 dB). This unit's echo-only ratio starts above
`dtd_init`: the tracked threshold has to rise before it adapts, which it
may while the gate is open on the cold residual (the filter's hot-mu
excess, which decays only when it adapts), and the divergence escape
(check 27) also catches it. `-DAEC_DTD_COLD_RISE=0
-DAEC_DTD_ESCAPE_BLOCKS=0` (no rise while the gate is open, no escape) must
FAIL (b): the gate opens on the cold residual, the tracker stays frozen and
x1 stays at 0.0 dB. `AEC_GS_NOAPI=1 ./test_aec_fd` leaves the scale unset
and both must FAIL. It also checks the setter: diag reads back the factor,
NaN, inf, 0, negative, under 0.001 (a denormal) and over 100 are rejected,
0.001 and 100 accepted.

Check 25 covers the gate's high-band reference term (`gate_kappa_hf`,
2026-10-06; Halo 04 worn): the speech-like reply restarts after each 0.5 s
pause on an 80 ms sibilant (+12 dB/oct noise, mostly above 4 kHz) ahead of
the voicing, and a distorting speaker adds white noise under the sibilant's
envelope at the mic. Four forked runs (distortion on / off x the term on /
off), each warm-up, echo-only reply, then the wearer over the reply. With
the term the gate stays shut on the echo (0/300 open); with
`gate_kappa_hf` 0 it releases at every restart and the hangover holds it
(282/300). The wearer's median out-in is within 1 dB with and without the
term (-1.1 / -1.4), and within 0.5 dB on a clean speaker (-1.2 / -1.2).
`-DAEC_SUP_GATE_KAPPA_HF=0.0f` must FAIL. The runs pin the shipped tracked
double-talk threshold (`dtd_mult` 10, `dtd_init` 0.25), so the check tests
the term alone: with the fixed threshold the filter adapts on the
distortion bursts and the gate opens on 143/300 echo-only blocks even with
the term (232 without; measured at the 2026-10-06 defaults).

Check 26 covers the tracked double-talk threshold (`dtd_mult`,
`dtd_init`): check 13's double talk with the tracked threshold (defaults)
and with the fixed `p_err < 2 p_ref` (`dtd_mult` 0), forked. After the
convergence phase the tracked threshold sits at its 0.05 floor (this
synthetic echo is almost perfectly linear), the wearer's median out-in is
-0.5 vs -1.2 dB, and right after the double talk the tracked run still
cancels 18.9 dB vs 5.4 for the fixed one, whose filter adapted to the
wearer. `-DAEC_DTD_MULT=0.0f` must FAIL. (b) A x2.5 gain change (mic and
scale) mid-reply on check 24b's strong-coupling unit, at the defaults (the
threshold at its floor) and at `dtd_mult` 100 (threshold ~0.25, above the
floor, where a lost ratio shows): cancellation over the 1 s after the change
must stay within 2 dB of the second before it (23.1 -> 24.7 dB), and
`diag('stats').dtd_thr` (gain-1 units) within 5% (0.250 -> 0.252).
`-DAEC_GAIN_RESCALE=0` (filter and power trackers not carried over) must
FAIL (23.1 -> 13.1 dB: the residual reads as double talk and adaptation
freezes), and so must an empty `aec_dtd_rescale` (threshold 0.250 ->
0.050).

Check 27 covers an echo-path change after convergence, on check 24b's
strong-coupling unit and check 24a's hot unit: a new impulse response (the
glasses re-seated) and the coupling x3. Forked runs with the tracked and
the fixed (`dtd_mult` 0) threshold; cancellation 2-4 s after the change
must be within 3 dB of the fixed threshold's (17.7 vs 18.0, 23.8 vs 24.3,
17.2 vs 17.5, 23.7 vs 24.5 dB). The residual jumps far above the tracked
threshold (at its floor) and adaptation freezes; the divergence escape
(`AEC_DTD_ESCAPE_BLOCKS` 40 frozen playback blocks (0.8 s) with the
residual coherent with the reference, coherence >= 0.15) falls back to the
fixed threshold and the tracker decays back as the filter re-converges.
Before the fix the review measured -6 dB (new path) and +0.6 dB (x3, linear
stage) for 12+ s. `-DAEC_DTD_ESCAPE_BLOCKS=0` must FAIL (11.9 / 6.9 dB).
(c) Anti-windup: a linear unit converged for 60 s (its ratio ~50x under the
floor), then the mic noise floor steps to ~-4 dB under the reference
(incoherent, so the escape cannot fire): adaptation must resume within ~15
s (frozen on under 1/4 of the blocks 18-23 s after; 0/250). The tracker is
clamped to the threshold's bounds; `-DAEC_DTD_CLAMP=0` must FAIL (250/250:
the unclamped tracker climbs from far below the floor, ~30 s longer).
(d) No carry-over between replies: the strong-coupling unit's coupling
goes x3 30 blocks before a reply ends (a frozen-coherent run of 24, short
of the escape) or right at its end, then 0.6 s of speaker idle (bypass) and
the next reply. The re-engage wipes the reference history and with it the
escape run and the coherence accumulators (`aec_dtd_esc_reset`, also at
enable, a resync and a mic-sample loss): the run is never longer than the
new reply (`dtd_run` in `struct audio_aec_stats`) and the reply-2 coherence
matches the no-run case (max diff 0.001). Without the resets at the wipe
sites it FAILS (run 32, coherence diff 0.12).

Check 28 guards the escape against the wearer: 12 s of continuous double
talk (`near2_sample`, 250 ms syllables / 100 ms gaps, no pause) at two
levels on the strong-coupling, hot and linear units. The residual is the
wearer, incoherent with the reference (0.01-0.07), so no escape may fire,
and cancellation right after must be within 3 dB of before (worst -1.0 dB;
the filter is frozen on 0-99% of the double-talk blocks by level). The
residual coherence on frozen blocks is the discriminator: an echo-path
change reads 0.27-0.40. Before choosing it, a gate-shut test was tried:
the gate itself lets go of a loud continuous talker after ~3.5 s, so it
fired there. `-DAEC_DTD_ESC_COH=0.0f` (escape on frozen alone) must FAIL
(10 escapes, -15.1 dB after).

The device builds audio_aec.c with `-O3 -ffast-math`, under which GCC
assumes no NaN and folds a plain `!(v >= min && v <= max)` so that NaN
passes it; `audio_aec_tune_check()` therefore tests the float bit pattern.
To run the suite the way the device compiles the file, add the flags to the
FDAF build (all checks pass that way too):

    gcc -O3 -ffast-math -Wall ... -DCONFIG_HALO_AUDIO_AEC_FDAF=1 ...

Apple clang on arm64 does not reproduce the fold (its inverted compare
happens to reject NaN), so on a Mac the NaN rejection is only proven on
the device compiler: build audio_aec.c with arm-zephyr-eabi-gcc and the
flags from `build/halo/compile_commands.json`, and check in the
disassembly of `audio_aec_tune_check` that the exponent test (`0x7f800000`)
comes before the float compares. A GCC host build with `-ffast-math`
exercises it directly.

`AEC_TUNE="key=value,..." ./test_aec_fd` applies a runtime set before
the first block. `./tune_equiv.sh` builds the FDAF harness with a `-D`
override and diffs its whole output against the default build run with
the equivalent `AEC_TUNE` (check 23 excluded, since it reads the
defaults): `rearm_ms=160` vs `-DAEC_SUP_ONSET_REARM_MS=160` (check 19
fails identically, 6 re-arms), `gate_kappa=0.3` vs
`-DAEC_SUP_GATE_KAPPA=0.3f`, a two-key gate set, the gate band and the
two-band ceiling switched off, a different split and low-band cap, the
top cap band off, at 1.6kHz / 0.1, at 2kHz / 0.15 and at 1kHz / 0.05 with
the low split off, the whole pre-2026-10-04 set, the gate hang at its old
1200 ms, the high-band term off and at 0.3 / 3.8kHz / ratio 4, the fixed
double-talk threshold (`dtd_mult` 0), `dtd_mult` 20 with `dtd_init` 0.5,
and an explicit default. All must print SAME.

Both filter cores build from the same file:

    cd applications/halo/tests/aec/host
    # time-domain NLMS build
    gcc -O2 -Wall -I. -I ../../../../../modules/halo/include \
        -DCONFIG_HALO_AUDIO_AEC_TAPS=1024 -DCONFIG_HALO_LOG_LEVEL=3 \
        -o test_aec ../../../../../modules/halo/src/audio_aec.c test_aec.c -lm
    ./test_aec
    # per-bin FDAF build (the device default; plain-C FFT stands in
    # for CMSIS-DSP with identical packing/scaling conventions)
    gcc -O2 -Wall -I. -I ../../../../../modules/halo/include \
        -DCONFIG_HALO_AUDIO_AEC_TAPS=1024 -DCONFIG_HALO_LOG_LEVEL=3 \
        -DCONFIG_HALO_AUDIO_AEC_FDAF=1 -DCONFIG_HALO_AUDIO_AEC_FDAF_PARTS=3 \
        -o test_aec_fd ../../../../../modules/halo/src/audio_aec.c test_aec.c -lm
    ./test_aec_fd

All checks must PASS in both builds (checks 17 and up, and the INFO
lines, run in the FDAF build only). The FDAF build is held to
higher thresholds where its per-bin adaptation (and its suppressor/onset
stage) is the point: >14dB on noise convergence (TD: >10), >8dB voiced
(TD plateau: >3), >6dB cold-onset (TD: >2) and >12dB warm re-engage
(TD: >4 - it refills its cleared ref history through ~64ms of
uncancelled passthrough at re-engage and has no suppressor to cover
that).

`./test_aec -v` prints a per-block trace of the convergence run.

Failing controls (prove the checks discriminate):

- `-DAEC_EPOCH_FREEZE_MS=999999999` disables the epoch freeze; the jitter
  and pair checks must FAIL in either build (resync +1, ERLE collapse, lag
  shifted to the re-anchored base).
- `-DSC14_TRUE_GAPS` runs check 14 with real feed gaps (the pre-silence-
  feed driver: emission stops at every reply gap). The tap-jitter and
  combo recipes MUST fail (~5dB vs ~13): each gap forces the emission
  epoch to be re-learned from ms-quantized callback timestamps, a
  per-reply shift of even 1ms (16 samples) rotates the per-bin phase
  ~180 deg at 500Hz, and the misaligned residual then reads as
  double-talk so adaptation never recovers within the reply. This is
  the live BLE-duplex failure measured on the Halo unit (in-band ~0dB across
  16 reply spans, zero resyncs, converged w, self-triggered barge-ins)
  and the reason the driver silence-feed exists.
- `-DSC15_NO_LOSS_HANDLING -DAEC_LATE_FLOOR_MS=1000000` runs check 15
  with the pre-fix behaviour (losses never reported, late-floor backstop
  disabled). BOTH parts must fail in either build with the device's
  exact death signature: margin_max +320 per dropped block (4800 after
  15), resyncs stuck, cap_slips 0, ERLE ~0 once the cumulative slip
  passes the window span.
- `-DAEC_LATE_FLOOR_FWD_ONLY` restores the forward-only late-floor
  backstop (drops the symmetric earlier-slide). Check 15 part (c) MUST
  fail in either build: cap_slips stuck at 0 and cap_late_ms climbing
  without bound (~89k ms over the 20s recovery vs ~7.5k frozen with the
  fix) as the too-late anchor never re-anchors - the session-212454
  walk-off crush.
- FDAF build: drop the onset / fail-safe clears (`sup.onset`,
  `ref_unrel_hops`, and in the bypass `gate_hangover`) from the session
  reset and the speaker-idle bypass: check 17 MUST fail in both paths
  (`left 49`, near blocks cut by more than 10dB in each).
- TD build: `-DAEC_LPF_ALPHA=1.0f` (no update band-limit) fails the
  HF-noise check.
- FDAF build: `-DAEC_FD_MU=1.0f` fails the HF-noise check (linear stage
  8.5dB vs 14.2 at the default 0.25, limit 10), and with it double talk
  (-3.3dB) and check 20 (ERLE 11.5dB); voiced drops 14.9 -> 11.4dB.
  `-DAEC_FD_LEAK=1.0f` costs ~1dB voiced.
- FDAF build, onset stage: `-DAEC_SUP_BETA=0.0f` (suppressor passthrough)
  fails the cold-onset check (5.5 vs 6.8dB); `-DAEC_FD_MU_HOT_EXCESS=0.0f`
  (old soft-start pace) fails it harder (4.2dB); `-DAEC_SUP_BETA=6.0f`
  (over-aggressive) fails the double-talk check (-4.5dB median vs the
  -3dB bound; default 1.25 sits at -1.3).
- FDAF build, onset re-arm: `-DAEC_SUP_ONSET_REARM_MS=160` (the old
  re-arm after the 160ms adaptation hangover) fails check 19 (6 re-arms;
  7/370 near blocks < -15dB with the gate lift, 58/370 with the item-5
  gate);
  `-DAEC_SUP_ONSET_GATE_LIFT=0` (the duck caps the wearer too) fails
  check 22 (11/25, median -13.4dB); `-DAEC_SUP_ONSET_REARM_MS=2000`
  (longer than check 20's gap) fails checks 20 and 22 (duck not armed).

FDAF-specific regression worth knowing about: the update views (error and
reference alike) MUST be streaming-filtered (IIR across block boundaries)
BEFORE the FFT's rectangular frame gating - gating the raw error leaks the
rumble across the entire adaptation band (~6dB/oct sidelobes) and collapses
in-band ERLE from ~12dB to ~2.5dB even though the rumble is far below the
adaptation band. A per-bin mask cannot remove what the gating has already
spread; this is the frequency-domain restatement of the time-domain build's
filtered-error design.

FDAF-specific regression #2, found porting the onset stage: adaptation
must be WITHHELD while the reference history still contains a wipe's
hard zero edge (~6 hops after enable/resync, `FD_HIST_FILL_HOPS`) - the
edge's rectangular-gating leakage poisons the per-bin gradient exactly
like the rumble did, and at hot mu the damage lands in W (measured here:
cold-onset ERLE -3.3dB vs +1.3 old-ramp without the guard; warm
re-engage 19.5dB with it vs 7.8 without, because every reply-gap resync
was corrupting the converged filter for its first 120ms). Prediction
and subtraction stay on through the fill - the pre-wipe silence is real.
