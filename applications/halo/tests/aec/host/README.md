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

All sixteen checks must PASS in both builds. The FDAF build is held to
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
- TD build: `-DAEC_LPF_ALPHA=1.0f` (no update band-limit) fails the
  HF-noise check.
- FDAF build: `-DAEC_FD_MU=1.0f` degrades voice/HF markedly (7.3/8.6dB vs
  10.9/14.3 at the default 0.25); `-DAEC_FD_LEAK=1.0f` costs ~1dB voiced.
- FDAF build, onset stage: `-DAEC_SUP_BETA=0.0f` (suppressor passthrough)
  fails the cold-onset check (5.5 vs 6.8dB); `-DAEC_FD_MU_HOT_EXCESS=0.0f`
  (old soft-start pace) fails it harder (4.2dB); `-DAEC_SUP_BETA=6.0f`
  (over-aggressive) fails the double-talk check (-4.3dB median vs the
  -3dB bound; default 1.5 sits at -1.5).

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
