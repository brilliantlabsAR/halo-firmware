# Acoustic echo cancellation (AEC) — offline prototyping + firmware port plan

> **Current on-device mic AEC/voice API** (this section is authoritative; the
> session journal below is historical and predates the 2026-07-15 cleanup).
> The Lua surface is now regular — bool getter/setter pairs mirroring `gain()`,
> with diagnostics split onto their own function. `aec` and `voice` are two
> independent, **opt-in (default off)** stages on the mic path
> (`mic → [aec] → [voice] → encode`); set them declaratively at
> `start{aec=, voice=}` or toggle live:
>
> | Call | Meaning |
> |------|---------|
> | `frame.microphone.aec()` / `aec(bool)` | get / set echo cancellation (live); also `start{aec=true}` |
> | `frame.microphone.voice()` / `voice(bool)` | get / set voice-band mode (live); also `start{voice=true}` |
> | `frame.microphone.diag('stats')` | canceller + PDM/speaker/clock diagnostics table |
> | `frame.microphone.diag('zero')` | zero the clkmon / PDM / speaker counters |
>
> Removed in the cleanup: `aec('sup'/'nosup')` (dead), `aec('pair')` and
> `aec('dump')` (retired with the `pair_probe.py` / `dump_probe.py` /
> `diag_voice_pair.py` / `chained_search.py` probes). Older references to
> `aec('stats')`, `aec('pair')`, etc. in the journal below reflect the API at
> the time of writing.

The Halo's bone-conduction speaker leaks into the mics on the same frame. Any
duplex voice loop (e.g. the flutter `realtime_gemini` example talking to the
Gemini Live API) re-sends that echo as if the wearer had spoken, so barge-in is
unusable without cancellation. AEC belongs **in firmware**: mic PCM and speaker
PCM are both available here, on a short, tight, drift-bounded path — instead of
the phone-side loop where the echo shows up 100–600 ms late through two LC3
codecs and BLE.

**Ordering with beamforming:** AEC runs **before** the dual-mic beamformer
(`../beamform/`). The beamformer's masks would otherwise be estimated on
echo-contaminated inputs — and the echo source (the speaker on the frame) is
exactly the kind of coherent near-field signal the beamformer can't reject.
As of this branch the beamformer exists only as an offline harness (no C
stage in the mic path), so there is nothing to disable during AEC bring-up;
when it is ported, the chain is `mic → AEC → beamform → LC3`.

## Offline harness

`aec.py` runs the full prototype pipeline on an aligned `(mic, reference)` WAV
pair (16 kHz mono PCM16):

```
uv run aec.py aec_mic.wav aec_speaker_ref.wav
```

1. **Bulk delay** — GCC-PHAT, global + windowed (drift check).
2. **Linear NLMS FIR** (default 640 taps = 40 ms) with a double-talk detector
   that freezes adaptation while the wearer speaks. This stage is the direct
   C port candidate: block NLMS on int16 at 16 kHz needs no FFT and no
   CMSIS-DSP to start.
3. **Residual suppression** — STFT over-subtraction gated by the *predicted*
   echo. This targets the nonlinear residual: `spk_protect.c` (the active
   post-volume protection EQ/limiter) and the transducer itself distort, and
   phone-side measurements show the mic picks up ~+12 dB more HF (relative to
   LF) than the reference contains during loud playback. A linear FIR cannot
   subtract that; suppression (or a nonlinear model) mops it up.

### Getting capture pairs

- **Realistic geometry (Halo unit + iPhone):** the `realtime_gemini` example app
  captures `(aec_mic.wav, aec_speaker_ref.wav)` during a conversation, and its
  *Echo sweep* button plays noise bursts + a 5 s exponential sine sweep through
  the LC3→speaker path while recording the mic. Deconvolving the sweep (Farina)
  yields the echo path's linear impulse response — which sets the FIR length —
  and separates harmonic distortion orders (see
  `brilliant_sdk/recordings/analyze_sweep.py`). Prefer far-end-only captures
  (wearer silent) for path identification.
- **Device-exact timing (dev kit):** play a known clip with
  `../test_speaker_lc3.py` while recording the mic to `/lfs` with
  `../record_mic_lfs.py` (~1.5 s cap at 16 k stereo). Short, but measured at
  the real firmware taps with no BLE/LC3 in the capture loop.

## Echo-path identification results (Halo unit, head-worn, 2026-07-11)

From the app's echo sweeps at volume 100/80/60/40 and mic gain −5/0/+5
(`recordings/` in the brilliant_sdk workspace, `analyze_sweep.py`):

- **The echo is strong and clean**: +32…+39 dB above the mic noise floor
  along the sweep track. Cancellation is very much worth doing.
- **The linear IR is compact**: 90% of energy within ~12–38 ms of onset,
  99% within ~60–130 ms → a **512–1024-tap FIR at 16 kHz** covers the
  linear path; most energy is in the first few hundred taps
  (structure-borne + short acoustic path).
- **Harmonic distortion** (Farina separation, relative to the linear IR):
  H2 ≈ −20 dB, H3 ≈ −16 dB at v100/g0, similar at v80/v60 → the **linear
  cancellation ceiling is ~13–16 dB ERLE**; a residual echo suppressor is
  needed beyond that (as `aec.py` stage 3 prototypes).
- **The phone loop cannot validate subtraction quality**: the reference-to-
  emission delay *wanders ±20 ms on second timescales* (BLE pacing + the
  speaker's internal buffer occupancy: 90→112→108 ms within one sweep;
  67–119 ms across a far-end capture), so static and NLMS cancellation both
  fail through that loop no matter how good the filter is. The IR shape,
  distortion levels and echo-to-noise above are still valid (measured
  within short windows); the ERLE numbers are only attainable with the
  drift-free firmware taps — which is the point of doing AEC here.
- **Sustained loud playback browns out the device**: head-worn far-end
  playback at volume 100 and 80 shut the Halo down at 68% battery
  (battery-protection IC; charger-connected works around it). Separate
  problem from AEC, but it means `spk_protect` limiting is likely to be
  active exactly in the AEC operating regime — one more reason the
  reference must be tapped post-`spk_protect_process()`.

## Duplex "mic corruption" post-mortem (2026-07-11, resolved)

The bring-up scare documented in commit 26bbfff — mic PCM apparently turning
into loud spectrally-flat noise during simultaneous playback — was **not a
firmware signal-path bug**. Byte-exact `/lfs` captures decode frame-aligned
(offset-0 beats every other byte offset on stimulus correlation in every
window), and a same-metric comparison against the flutter far-end captures on
*released* firmware shows statistically identical duplex behaviour (median
speech-band stimulus-NCC 0.25 vs 0.29, same octave-band profile). What
actually happened, in layers:

1. **BLE-streamed captures** (`loopback_test.py`) lose notification bytes
   under duplex BLE load; any non-multiple-of-40 loss shifts the host
   decoder's LC3 frame grid and everything after decodes as loud flat noise.
   Those were the captures being listened to.
2. **All mic captures are dominated by constant sub-100 Hz rumble**
   (>90% of full-band energy, ~−26 dBFS, present on released firmware and in
   quiet-room mic-only captures too), which blinded full-band RMS metrics and
   envelope detectors. Band-pass before measuring anything.
3. A real latent firmware bug was found along the way — the Lua mic ring's
   overflow discard was byte-granular, which would break the LC3 frame grid
   on-device if the 64 KB ring (≈16 s mono LC3) ever overflowed — but it
   cannot have triggered in these short captures. Fixed regardless
   (grid-preserving whole-put-unit discard); harness reads are now also
   40-multiples like the flutter app's.

**Valid on-device results after the cleanup (Halo unit, on desk, v100,
quiet room, 2×12 s runs):** AEC-off duplex echo sits at −38…−43 dBFS
speech-band over a −72 dBFS floor, and by ear is normal speech. AEC-on stays
clearly audible speech with an added "alien" artifact, and the numbers agree
(pop-excluded scoring, see below): **steady-state ERLE ≈ 0 dB** (+0.1/−0.1
across runs — the filter removes coherent echo but adds artifact energy of
equal power) and an **adaptation-divergence burst at 1.3–2.1 s of playback**
peaking −22 dBFS (≈ +15 dB over the echo; onset-window ERLE −8 dB). The
burst is bin-for-bin deterministic across runs (same stimulus → same
trajectory to ±0.2 dB), so it will be exactly reproducible while fixing.
One of the two runs also injected noise into the post-playback silence
(−57 vs −72 dBFS floor) — the filter convolving stale reference history
after the FIFO drains; state-dependent, not constant. AEC-on windows show
*lower* stimulus coherence than AEC-off, so the linear subtraction path
works — onset handling and idle bypass are what need fixing. The earlier
"+7.8/+10.5 dB ERLE" figures were measured through garbled BLE-decoded
captures and are void.

Measurement hygiene (all handled by `loopback_lfs.py score()` now): the mic
start emits a **PDM pop** (~−28 dBFS, first ~0.5 s of every capture) and
mic-stop puts a click in the last bin — both are excluded from the scored
region; scoring reports onset (first 1.5 s of playback) and steady-state
ERLE separately; `--seconds` defaults to the full 10 s stimulus (the old 8 s
default truncated it mid-sentence, which read as a "speaker cutout" at the
end of captures).

Next: gate adaptation and subtraction on reference power (bypass when the
FIFO underruns / ref is silent), ramp mu at onset, clamp the filter output
relative to the mic block, then re-measure — head-worn as well as on-desk.

## Clock audit: the "~0.5% clock skew" was NOT a clock (2026-07-12, resolved)

After the stabilization pass (commits 7b226c6..d9e8dd5: filtered-error
NLMS, ref gating, mu ramp, emission-time reference feed) the canceller is
burst-free, injection-free and structurally correct — the `aec('dump')`
tap readout shows a causal impulse response at ~13 ms depth — but
steady-state ERLE stayed ≈ −4 dB with ‖w‖² ~20× oversized/diffuse, and the
mic-vs-stimulus lag climbed **+5.1 ms/s** in every duplex capture. That was
initially attributed to a PDM-vs-I2S sample-rate mismatch. It is not one.

**Direct measurement (dev kit, `clock_rate_test.py` + the `aec('stats')`
clock probes and PDM-driver ledger):** per-stream sample counters against
the crystal-referenced kernel clock, self-paced local silence playback (no
BLE audio in either direction), 60 s and 120 s windows:

- **I2S speaker: 16000.27 Hz (+17 ppm)** in every phase — crystal-exact.
- **PDM mic: 16000.0 Hz** once the constant ≈2,500-sample (~8-block,
  ~160 ms) start-of-session consumption backlog is subtracted — the excess
  does not scale with duration (2,592 samples at 60 s, 2,608 at 120 s), so
  it is a start transient, not a rate. Between-session differencing gives
  16000.24 Hz. FIFO-count sanity probes (`stat_gt8`) show zero glitches;
  zero overflows, zero drops.
- Registers (dumped in `aec('stats')`): both peripherals on the shared
  crystal-derived `76M8_CLK` (`I2S0_CTRL[16]=0`, `HE_CLK_ENA[9]=0`,
  `OSC_CTRL[PERIPH_XTAL_SEL]=1`), I2S divider 150 = exact for mono 16 kHz.
  The B1 HWRM (docs/ in the workspace root) confirms both peripherals'
  functional clock is `76M8_CLK` or the (unused) fractional audio PLL.

So the PDM/I2S converters are locked to within ~20 ppm of each other, the
"0.5% playback pitch error" does not exist, and no resampler is needed.

**Re-attribution of the drift:** with exact clocks, the +5.1 ms/s
mic-vs-stimulus slope in the /lfs duplex captures must come from the
**emission timeline stretching — BLE stimulus feed starvation**. Those
captures streamed the stimulus over BLE (paced at 0.9× frame time) while
the capture loop also did /lfs writes on the Lua thread; every speaker
queue underrun inserts an emission gap, which the host-side lag analysis
reads exactly like a slow speaker clock. The AEC's perpetual re-convergence
on the Halo unit is the same story seen from the ref FIFO: each feed gap forces a
pad whose size is estimated, and per-gap estimation error walks the
alignment. (Loss on the capture side is ruled out by direction: dropped
capture samples would make the lag slope negative.)

Measurement-artifact traps hit along the way, so they are not rediscovered:
`k_cycle_get_32()` deltas lose time across idle sleep on this SoC (RTC
idle timer) and skew rates by hundreds of ppm — accumulate `k_uptime`
deltas instead (quantization telescopes away over a contiguous run); and
consumption-side counters see the start-of-session backlog burst as a rate
error unless the window is long or the backlog subtracted. Also found and
left in place: the PDM warn ISR fires twice per watermark event (the
second entry early-exits; inefficiency only), and `k_msgq_put` failure in
the PDM ISR leaks the block (now counted in the ledger, never observed).

## Alignment root causes (2026-07-12 night — fixed; steady-state WIP)

With clocks exact, three stacked alignment defects explained the
zero-ERLE history; all are fixed on this branch and validated by the
host harness (all five scenarios green):

1. **Consume-time anchoring.** The consumer paired each mic block with
   the newest reference at consume time, but the mic driver queue
   carries a variable 0–160 ms backlog (the session-start burst the
   clock probes measured), so the window sat up to 160 ms from the
   echo — often entirely outside the 64 ms tap span. Replaced with
   **physical-time pairing**: producer and consumer each keep a
   minimum-latency estimate of their stream's epoch (min over events of
   `now − samples/16k`, kernel-uptime based), and the window is
   anchored where emission time equals the mic block's reconstructed
   capture time. Drift > 160 samples re-anchors and clears the
   reference history (splicing old history bursts).
2. **Tap at DMA handoff.** Handoff leads emission by the queue
   occupancy — ~160 ms when a self-paced feed saturates the 8-block
   queue (the old BLE-paced feeds ran it shallow, masking this). The
   tap now fires from the **DMA completion callback**
   (`max98357a_audio.c`): a completed block has just finished emitting,
   which is emission-exact at any queue depth; the hold-one-block
   mechanism is gone.
3. **Stale-segment ring content.** After an emission gap the window's
   head read the previous session's ring content as if it were the
   gap's silence. The producer now fences `seg_start` at every >40 ms
   feed gap; pre-segment indices read as silence (which the gap truly
   was).

Also: the reference ring grew to 256 ms (covers max mic backlog +
window; paid from `CONFIG_HALO_MEM_INTERNAL_SIZE`), `AEC_REF_LEAD` is
352 (feed quantization + timestamp-jitter margin; ≥ 384 clips the
harness IR tail), and `aec('stats')` gained `ref_underruns/ref_pads`
and `feed_gaps/feed_gap_ms`. Test-harness hygiene that mattered:
`erle_local_test.py` buffers the capture in RAM during playback — /lfs
writes stall the Lua thread long enough to drain the DMA queue, i.e.
the old capture loop was injecting the very emission gaps under study.

**Hardware status (Halo unit, local noise stimulus, v80):** first
positive cancellation measured on-device — onset ERLE **+2.4 dB** — but
steady-state still ~0: the ERLE run logged 13 resyncs (anchor churn
under LC3 mic load, cause not yet isolated) and even a clean 0-resync
probe session leaves the learned IR diffuse across the window. A
truncated mid-vs-end IR comparison (first 128 taps only — the on-device
dump copy hit the ~512 B `f:read` cap) shows low correlation,
suggesting the filter is still slowly refitting.

**Next:** (1) fix the dump-copy chunking and compare full mid/end IRs to
confirm or kill "alignment still walks"; (2) if walking, log epoch
refinements (each min-estimator update) to see what moves after
convergence — consider freezing epochs once established; (3) add a
one-shot paired capture (mic block + consumed-ref history) to measure
the true echo lag in the window directly; (4) then the residual
suppressor and BLE-fed duplex re-test.

## Epoch freeze + paired capture (2026-07-12 — coded, awaiting hardware)

Items (1)–(3) above are done on this branch; the resync churn has a
confirmed structural cause:

- **The epoch estimators never stopped refining.** Both stream epochs
  are running minima; after the filter converges, any block that
  happens through scheduling luck to be consumed with lower latency
  than ever seen before refines its epoch. A sub-160-sample refinement
  only moves the drift measure, but refinements accumulate and each
  threshold crossing is a **resync — history wipe plus a shifted
  re-anchor — forcing a full refit** (the "13 resyncs under LC3 mic
  load"). With the clocks proven crystal-locked there is nothing
  legitimate left to track after establishment, so epochs now
  **freeze `AEC_EPOCH_FREEZE_MS` (2 s) after (re)establishment**; the
  residual latency bias is constant and absorbed by the FIR taps.
  Post-freeze refinements are counted and suppressed
  (`cap/emit_late(_ms)` in `aec('stats')`, applied ones in
  `cap/emit_refines(_ms)`) so hardware can validate the choice. Real
  timeline changes (feed gaps, mic restarts) still reset the epoch
  entirely and re-open the window. Host harness scenario 6 reproduces
  the churn (constant 12 ms consumer latency, one lucky 0 ms block):
  unfrozen it resyncs and ERLE collapses 10.4 → 4.5 dB; frozen it
  holds. All seven host scenarios green.
- **`aec('pair')`** arms a one-shot snapshot of a raw mic block plus
  the exact raw reference window it was cancelled against, returned as
  a binary Lua string (`[n:u16][taps:u16][mic i16×n][ref i16×taps+n]`;
  no /lfs write — flash writes stall the Lua thread and drain the DMA
  queue). Cross-correlating first-differenced copies (rumble drops
  out) reads the true echo lag in tap coordinates, directly comparable
  to the learned |w| profile. Host-validated to 1 tap against the
  synthetic IR (matched filter, scenario 7); when the freeze is
  disabled the measured lag shifts exactly to the re-anchored base, so
  the probe sees anchor moves as designed.
- **`pair_probe.py`** runs a gap-free duplex noise session capturing a
  pair every N blocks (buffered in Lua RAM, pulled after playback) and
  prints the lag trajectory: stable lag ⇒ alignment holds and any ERLE
  shortfall is the filter; walking lag ⇒ the epoch stats say which
  side moved. `dump_probe.py`'s mid-session tap copy is fixed (chunked
  `f:read` — the single 4096-byte read silently truncated to 128 taps,
  which voids the earlier "low mid-vs-end IR correlation" observation).
- The pair snapshot buffers (~3.3 KB) are paid for from
  `CONFIG_HALO_MEM_INTERNAL_SIZE` (−4 KB), since link slack was ~2 KB.

**Next:** (1) flash the dev kit, run `pair_probe.py` + `erle_local_test.py`
— expect resyncs ≈ 0, stable pair lag, and steady-state ERLE finally
positive; check `cap_late_ms`/`emit_late_ms` for what the freeze
suppressed; (2) re-run `dump_probe.py` (fixed copy) for a true mid-vs-end
IR comparison; (3) if steady-state ERLE is still ~0 with a stable lag,
the problem is the filter, not alignment — look at mu/DTD under LC3
load; (4) then the residual suppressor and BLE-fed duplex re-test.

## The dev kit hardware session (2026-07-12 night — alignment PROVEN, the dev kit can't measure ERLE)

Ran on the dev kit with the epoch freeze + paired capture + margin stats
flashed (plus `CONFIG_LOG_BACKEND_FS=n` — the FS log backend writes hit
the same flash the code XIPs from, i.e. the logging thread was one more
`/lfs`-stall source no harness hygiene could avoid; `lua_log.c` is now
compiled only with the backend).

**Alignment is verified stationary — this chapter is closed:**

- Epochs establish once and never refine (`cap/emit_refines` and
  `_late` all zero across every session — the freeze wasn't even
  exercised); zero resyncs everywhere.
- Mid-session window margins (new `margin_last/min/max` stats, sampled
  inside the play loop) sit at +203…+539 samples with **zero pads**:
  the window always ends on already-emitted reference with headroom,
  and the 320-sample margin range is exactly the block commit phase.
  (The earlier scary underrun/pad counts — e.g. 102/32320 in an ERLE
  run — were an artifact of reading stats after the play loop: once
  the feed stops, the still-running mic pads ~320/block. Sample stats
  mid-loop.)
- `aec('pair')` cluster-averaged correlation profiles (20–30 blocks
  per burst) shift **0 taps** between bursts seconds apart, with
  profile cross-correlation 0.80–0.83 — the whole (weak, spread) echo
  structure is bit-stationary. Note: per-block argmax on a weak echo
  hops between IR lobes (spreads of 500+ taps look like "walking");
  the probe's verdict now uses the averaged-profile shift instead.

**Why dev-kit steady-state ERLE is still ~0 — and always will be:** a ridge
fit of the *optimal* linear window→mic predictor over 60 captured
pairs (perfectly aligned by construction, `--save` + offline fit)
achieves only **+0.8 dB held-out in-band ERLE**, with fitted
‖w‖² ≈ 0.00125 ≈ the on-device converged `w_norm2`. The NLMS *had
converged to the optimum*; the dev kit's in-band playback energy
(−56 dBFS over a −72 floor at v100) is simply not linearly coherent
with the reference — nonlinear buzz/rattle, not linear echo. The dev kit
validates alignment and timing (its job as the wired dev kit), but
**cannot demonstrate cancellation quality**. That needs the Halo unit's real
bone-conduction path (measured linear IR: echo +32…39 dB, H2 −20 dB →
13–16 dB linear ceiling).

**Also landed, motivated by an dev-kit measurement that generalizes:** the
update domain was full-band (300 Hz–8 kHz) while the mic's non-echo HF
energy (PDM/ambient) sat ~10 dB above the echo there — full-band
gradients are mostly weight noise (measured w_norm2 ~6× the optimum
before the fix). The filtered-error views now get three cascaded
one-pole low-passes at ~3.8 kHz (same filter on error and reference
history keeps the in-band fixed point unbiased); subtraction stays
full-band. Host harness scenario 8 (HF-dominated mic noise,
device-realistic) shows 6.8 dB vs 2.7 dB without the band-limit; host
scoring is now band-passed 300–3400 like the device scoring, and the
synthetic speaker path got a realistic HF rolloff. All eight scenarios
green; `-DAEC_LPF_ALPHA=1.0f` and `-DAEC_EPOCH_FREEZE_MS=999999999`
are the failing controls.

**Next:** (1) flash the Halo unit (**needs explicit per-flash approval**) and
re-run `erle_local_test.py` + `pair_probe.py` head-worn and on-desk —
alignment fixes + band-limited update against a real linear echo path
should finally show steady-state ERLE approaching the 13–16 dB linear
ceiling; (2) consider a do-no-harm guard (leak w when p_err
persistently exceeds p_mic) so a converged-but-useless filter — the dev kit
case, or a muted/quiet session — never injects its ~2 dB of
misadjustment noise; (3) then the residual suppressor and the BLE-fed
duplex re-test.

## THE root cause, and first real cancellation (2026-07-13, the Halo unit)

Halo unit (approved + flashed) reproduced the dev kit's numbers exactly:
alignment stationary, but ERLE −2.2 dB and an offline optimal-linear
fit capped at +0.76 dB — on a device with a sweep-proven strong linear
echo. Two devices agreeing on ~+0.75 dB "physics" smelled like a
pipeline artifact, and it was — but in the window placement, not the
capture (consecutive `pair` windows overlap byte-exact, so bursts
chain into a continuous reference timeline):

**`chained_search.py`** (self-tested on synthetic delays) searches the
mic-vs-reference delay over ±150 ms *including the acausal side*, and
every dataset — both devices, three stimuli — showed one strong peak:

    Halo unit white v80:  d = −335 taps (contrast 8.5×)
    Halo unit smooth v80: d = −320 taps (6.8×)
    Halo unit white v40:  d = −321 taps (9.6×)
    dev kit white v100: d = −326 taps (5.7×)

**The echo sat one block past the acausal edge of the window.** The
true physical emission-to-capture delay is only **~1–2 ms** — not the
~44 ms assumed since the consume-time-anchor era — so `AEC_REF_LEAD =
352` pulled the entire 64 ms window ~20 ms past the echo. Every
symptom follows: +0.8 dB "optimal" fits (the echo isn't in the fitted
window), weight-noise `w`, ERLE ≤ 0, in-window ridges that were
deterministic stimulus artifacts. The dev kit "has no linear echo"
conclusion below is therefore **retracted** — the dev kit was never given a
window containing its echo (its true linear ceiling is unmeasured; the
alignment-stationarity findings all stand).

Since the physical delay is *below the tap's 20 ms commit granularity*,
a mic block's echo reference is not yet committed when the block
arrives — no window placement can fix that. The AEC now **holds each
mic block back one block** (20 ms mic latency) and cancels it against
the next arrival's window; accounting stays on the arriving block
(epochs unbiased, A/B-toggle-safe); `AEC_REF_LEAD` drops to 192,
placing the echo at d ≈ +145.

**Result (Halo unit, on desk, local noise):** pair ridge at d = 141 in
window, contrast 8×, profile corr 0.97 across clusters — and

    v80:  ERLE onset +7.5 dB, steady-state +9.9 dB
    v100: ERLE onset +7.2 dB, steady-state +9.8 dB

with zero resyncs — against the 13–16 dB linear ceiling, using a
white-noise stimulus that drives the transducer's nonlinearity much
harder than speech playback will. First real on-device cancellation.

**Head-worn (same night):** ridge at d = 151–161 (10 ms, contrast 6×,
alignment holds), and head loading damps the nonlinearity:

    v60: ERLE onset +8.5 dB, steady-state +12.5 dB
    v80: ERLE onset +8.0 dB, steady-state +13.7 dB

— steady-state is inside the sweep-derived 13–16 dB linear ceiling.

dev kit re-measured with the corrected window: **+1.8 onset / +1.1 dB
steady** (was −2.2) — its echo really is mostly nonlinear/weak, but the
canceller now nets positive rather than injecting, even on a
nearly-echo-free device.

**Next:** (1) head-worn re-measure on the Halo unit (transducer loading
changes the IR); (2) `--lc3` pair run + duplex BLE re-test
(`realtime_gemini` barge-in is the goal); (3) the do-no-harm leak
(above) so no-echo sessions never inject misadjustment noise; (4) the
residual suppressor for the remaining nonlinear floor; (5) then
beamformer chaining.

## Real-voice stimulus (2026-07-13 ~4am): the speech problem, and the plateau

`erle_voice_test.py` plays a real Gemini far-end clip (LC3-encoded,
uploaded to Lua RAM, played through the speaker's LC3 mode - same
blocking back-pressure as PCM; note `duration` is in 10 µs units: 1000
= 10 ms, and the first run silently played nothing on `duration=10` -
the harness now fails loudly on Lua errors). Scoring is voice-activity
gated. Result on the noise-validated firmware: **voice-active ERLE
−1.9 dB with `w_norm2` GROWING to 2.4** (noise: +13.7, w ≈ 0.95).
Geometry exonerated: pair captures during voice playback put the ridge
at d = 136 (noise: 141), alignment stationary.

Host scenario 9 (pitch-pulse train through formant resonators,
wobbling f0, syllabic/utterance gating) reproduces it: voiced ERLE
1.5 dB, ‖w‖² 250× the true path. Mechanism: speech is pitch-periodic,
so period-shifted tap sets are gradient-indistinguishable and NLMS
spreads mass across ghost copies that misfire as the pitch moves.

Fixes landed (host-swept, device-verified):
- **IPNLMS-style proportionate steps** (`AEC_PNLMS_ALPHA` 0.75,
  mean-1 |w|-gains per block, capped 16×): concentrates adaptation on
  the compact IR; +2 dB on every noise scenario too.
- **Leaky LMS while adapting** (`AEC_W_LEAK` 0.94): drains ghost mass.
  Device-swept: 0.99 lets ghosts back (w 1.86, +0.9), no-leak is −1.9;
  0.94 is the working point.
- mu soft-start no longer resets at every gate close (utterance-gappy
  speech spent a third of its time at crippled mu); resync/enable
  still reset it.
- **Pre-emphasis tried and rejected on device evidence**: harness said
  +9 dB voiced, device said −1.6 - measured voice coherence FALLS with
  frequency (0.89 at 300–800 Hz → 0.39 at 1.6–3.4 kHz), so whitening
  re-weights the update toward the incoherent band. Default 0.

**Plateau (final config, head-worn, v80): voice-active +2.4 dB, no
injection, zero resyncs; noise regression +10.6 onset / +12.8 steady.**
The coherence spectrum bounds an LTI canceller at ~10 dB in 300–800 Hz
(where speech energy lives) falling to ~2 dB by 1.6 kHz — exploiting
that shape needs **per-bin adaptation (partitioned-block FDAF /
subband, the CMSIS-DSP stage)**, which is now clearly the next major
step for voice; the residual suppressor then handles the incoherent
remainder. Time-domain knobs are at their structural limit.

## Per-bin FDAF + speaker-idle bypass (2026-07-12/13 — coded, host + offline validated)

The CMSIS-DSP stage. Two changes on this branch:

1. **Speaker-idle bypass** (user requirement: AAD wakeups must add zero
   latency): the one-block hold-back and all filtering now engage only
   while the reference tap has fed within `AEC_SPK_HANGOVER_MS` (120 ms).
   Mic-only sessions are bit-exact zero-latency passthrough (host
   scenario 10); the filter taps stay warm across idle. Engage costs one
   block of inserted silence at playback onset; disengage drops the
   stale held block.
2. **Partitioned-block FDAF** (`CONFIG_HALO_AUDIO_AEC_FDAF`, default y):
   overlap-save, 1024-point CMSIS-DSP `arm_rfft_fast_f32` (Helium on the
   M55 — MVE confirmed enabled: `__ARM_FEATURE_MVE=3`), 20 ms hop, 3
   partitions × 320 taps. Per-bin NLMS (mu 0.25, swept) normalized by
   max(smoothed, instantaneous) per-bin power — smoothed-only lags
   speech envelopes and over-steps on every rise. Adaptation masked to
   bins 20–243 (~312 Hz–3.8 kHz); per-bin leak 0.998; alias-free
   round-robin gradient constraint doubles as the `aec('dump')` tap
   export. The ~50 KB state lives in the previously-empty 512 KB ITCM
   (DTCM has 6 KB slack left; section is NOLOAD → zeroed in sys_init;
   256 B guard below the state because ITCM starts at address 0).

   **The trap that cost the first implementation attempt**: update views
   (error AND reference) must be streaming-filtered before the FFT's
   rectangular frame gating — gating the raw error leaks the −26 dBFS
   rumble across the entire adaptation band (~6 dB/oct sidelobes),
   collapsing in-band ERLE 12 → 2.5 dB even though the rumble is far
   below the band. Same filter on both views keeps the per-bin fixed
   point unbiased (real |H|² scale, no phase rotation). This reuses the
   time-domain build's `xf` machinery; that build remains selectable
   (`CONFIG_HALO_AUDIO_AEC_FDAF=n`) and untouched in behaviour.

   Host harness (both builds all-green; FD held to higher thresholds):
   noise 17.6 dB (TD 12.1), **voiced scenario 9: 10.9 dB (TD plateau
   7.0)**, HF-noise 14.3 dB (TD 8.3).

   **Offline validation on real captures** (`offline_perbin.py`: numpy
   mirror of the firmware FDAF over voice_off_* sessions, LC3
   round-tripped stimulus, xcorr-aligned): **+4.5…+5.9 dB voice-active
   in-band ERLE vs the +2.4 dB TD device plateau**, per-band +7.3…+8.4
   at 300–800 Hz / ~+6.7 at 800–1600 / +2.2…+3.3 at 1.6–3.4 kHz — the
   coherence-shaped profile, as designed. The offline reference is
   pre-volume/pre-spk_protect, so the device tap should do at least
   this well.

**Hardware results (2026-07-14, both devices flashed + validated after
fixing an ITCM collision — halo_mem's region-1 heap lives at ITCM
0x1000, invisible in the linker map; the FDAF state now owns ITCM
[0,64K) with the heap offset moved up and a BUILD_ASSERT tying them):**

- the dev kit: noise +0.3/+1.2 dB onset/steady, voice +0.7 dB — its known
  weak-nonlinear-echo ceiling; zero resyncs/clamps, no injection, the
  floor even improved (bypass keeps post-playback pads bounded).
- **the Halo unit, on desk, v80: noise steady-state ERLE +15.3 dB** (TD build:
  +9.9) — at the sweep-derived 13–16 dB linear ceiling. Onset +3.3 dB
  (slower than TD's +7.5: mu 0.25 + soft-start; revisit if onset
  matters). **Voice-active +3.2 dB** — and the offline mirror on the
  same capture scores only +2.6 dB with the on-desk path measuring far
  less coherent (mean 0.23 vs 0.36–0.43 head-worn), i.e. the device
  beats its own offline optimum for the conditions; the +4.5…+5.9 dB
  offline prediction is for head-worn geometry and awaits a head-worn
  session.

**Head-worn (Halo unit, v80, same session):**

- **Voice-active ERLE +7.5 dB** (TD plateau: +2.4) — 3x, and above the
  offline mirror's +4.5…+5.9 dB prediction (the device tap includes
  spk_protect; the head-worn path is both stronger and more coherent
  than on-desk). Zero resyncs, no injection.
- **Noise steady-state +17.6 dB** (TD: +13.7) — above the 13–16 dB
  "linear ceiling" (per-bin partially tracks the level-correlated H2
  component; head loading damps the nonlinearity). Caveat: the off-pass
  floor was 11 dB noisier than the on-pass (ambient during lead-in) and
  the residual lands at that floor, so read it as "at least".

**Double-talk listening test (Halo unit, head-worn, wearer speaking over
the clip; voice_off/on_115311.wav):** the wearer's own voice comes
through unaffected while the assistant's voice is clearly pulled
down/altered - the DTD holds (zero resyncs, w stays small, no watery
artifacts, no burst at the talk-over transition). Score with wearer
speech in 257/280 frames still reads +4.4 dB.

**Barge-in gap found by ear (drives the next-session priority): the
first ~3 s of assistant-only playback sounds like plain voice** - the
cold-start convergence window (onset ERLE only +3.3-3.6). In a live
duplex loop that residual WILL trigger a false interruption at the
start of the first assistant reply. Mitigations, in likely order of
value: (a) the residual suppressor - it acts instantly, no convergence,
and the FDAF already pays for its spectra; (b) a faster onset schedule
(the max(P,inst) normalization already bounds the step, so the 0.5 s
mu soft-start and the 0.25 steady mu can likely be split: hot early
mu with settle-down, and skip the ramp entirely when W is warm - note
resyncs preserve W, only enable() wipes it, so real sessions re-engage
warm and the cold cost is once per mic session, worst-cased by the
erle harness's per-pass aec(false->true)); (c) app-side: gate the
interruption trigger for the first seconds of each reply.

**Next:** (1) onset: residual suppressor on the FDAF's existing spectra
+ faster warm/cold onset schedule (see above - this is the barge-in
blocker); (2) do-no-harm leak; (3) BLE-fed duplex re-test
(realtime_gemini barge-in); (4) beamformer chaining.

## Residual suppressor + onset schedule (2026-07-12 — the barge-in gap)

Two stages landed on this branch (FDAF build only), targeting the
"first ~3s of a reply pass uncancelled" gap:

1. **Per-bin residual suppressor** (`AEC_SUP_*`, `aec('sup'/'nosup')`,
   `sup_gmin` in stats): over-subtraction gains on the FDAF's output,
   per bin `g = max(1 - beta * Sy/Se, floor)` from smoothed
   predicted-echo (`|Y|^2` - the spectrum the filter already computes;
   validated equal to an exact prediction-history frame) and residual
   power. It needs no convergence of its own, so it covers both the
   linear filter's adaptation window and the nonlinear floor. The
   POWER-domain ratio (not amplitude, `aec.py` stage 3's rule) is
   deliberate: same echo suppression where predictions are good, half
   the double-talk damage where the wearer dominates (offline: -2 vs
   -4dB median on injected wearer speech). Gains are applied as a
   zero-phase kernel rotated to causal support [0, 2D], Hann-tapered,
   convolved directly over the residual history (alias-free by
   construction; D = 128 samples = 8ms group delay, playback-active
   only). beta 1.5, floor 0.1 (-20dB max).
2. **Onset schedule**: the 0.5s linear mu soft-start is replaced by a
   hot-mu excess (`AEC_FD_MU_HOT_EXCESS` 0.75 over the 0.25 steady mu)
   that decays exp per adapted block (~0.5s tau) and re-arms ONLY on a
   W wipe (= `enable()`), so every warm re-engage adapts at full mu
   from its first block. **The trap that made the first attempt
   backfire**: adaptation must be withheld while the reference history
   still contains a wipe's hard zero edge (~6 hops,
   `FD_HIST_FILL_HOPS`) - the edge's rectangular-gating leakage
   poisons the per-bin gradient like the rumble did, and at hot mu the
   damage lands in W. This guard alone lifted the host warm-re-engage
   scenario 7.8 -> 19.5dB: every reply-gap resync had been corrupting
   the converged filter for its first 120ms, on the old build too.

Validation so far (all pre-hardware):

- Host harness: three new scenarios - 11 cold-onset (speech stimulus,
  ERLE over the first 1.5s: 6.8dB vs 1.3 old; suppressor-off and
  hot-mu-off controls fail it), 12 warm re-engage (19.5dB immediate),
  13 double-talk near-end preservation (-1.5dB median on wearer speech
  ~5dB above the echo; beta=6 control fails). All 13 scenarios green
  in both builds.
- `offline_perbin.py` (firmware mirror, real head-worn voice_off_*
  captures): onset-1s ERLE +0.4..+0.9 -> **+5.1..+9.2dB**, onset-3s
  +0.5..+3.3 -> **+5.0..+12.5dB**, full voice-active up to +15.4dB
  (was +6.6 best). Injected near-end at -25dBFS attenuates only
  -1.8..-2.2dB median at the chosen beta. `--dt` and `--sweep` modes
  reproduce all of this; `--write-wavs` writes suppressed outputs for
  listening.
- `erle_voice_test.py` now also scores onset-1s/-3s windows.

**Hardware results (same day, both devices flashed + verified):**

- **the dev kit** (v100): noise +5.2 onset / +5.6 steady (previous build:
  +0.3/+1.2 - the suppressor floors even the dev kit's weak nonlinear
  echo); voice-active +5.0, onset-1s +4.9, onset-3s +4.0 - onset ~=
  steady, the gap is gone. Zero resyncs; `sup_gmin` 0.1 mid-playback
  (suppressor engaged); pair_probe: lag 166-167 taps stable, profile
  shift 0 -> alignment HOLDS on the new build.
- **the Halo unit, on desk** (v80): noise onset **+8.1** (was +3.3), steady
  **+29.3 dB** (was +15.3 - AEC-on output lands below the ambient
  floor); voice-active **+8.7 dB** (was +3.2), onset-1s **+4.8**,
  onset-3s **+7.6**. Zero resyncs. All numbers are worst-case COLD
  starts (the harness's aec(false->true) wipes W each pass); real
  sessions only pay that on the first reply.
- **the Halo unit, head-worn** (v80, quiet room): voice-active **+15.6 dB**
  (previous build: +7.5; TD plateau: +2.4), onset-1s **+7.8**,
  onset-3s **+10.9** - the first second of a cold reply now cancels
  better than the previous build's steady state. Noise onset +9.4,
  steady +39.6 (read as ">= ~21": the off-pass floor was 15 dB
  noisier - lead-in movement - and the on-pass output lands at its
  own -68 dBFS floor). Double-talk (wearer speaking over the clip):
  +10.0 voice-active with zero resyncs and w_norm2 steady at 0.05 -
  the DTD holds; the wearer-voice quality verdict is the listening
  test (voice_on_134924.wav). All cold starts, as above.

pair_probe.py note: each `aec('pair')` stalls the Lua feed 20-40ms
(its completion poll sleeps in the Lua thread), so pair iterations now
bank an extra play() first - without it a capture burst drains the
160ms DMA queue, the emission gap desyncs the AEC, and every later
pair call times out at 500ms while stalling further (measured 0/15
pairs, 13 gaps; with the bank 15/15, zero gaps). The clean duplex
loop (no pair calls) was spotless on the same build - if pair_probe
ever reports gaps again, check the probe before the firmware.

Also in this pass: `arm_rfft_fast_init_f32` -> the size-specific
`arm_rfft_fast_init_1024_f32` (the generic init links every FFT size's
tables: zephyr.signed.bin 678 -> 606KB), and the ITCM null guard is a
standalone `__used` symbol in the plain `ITCM` input section so it
actually sits at address 0 (inside the struct its placement was
compiler-chosen, and the linker had put the state first). ITCM total
59.8KB of the 64KB below the region-1 heap offset; the suppressor adds
~9KB there and nothing to main SRAM (a first cut with exact
prediction/residual spectra overflowed RAM by 11.6KB - the memory
trims above are measured-equal offline).

## Server-VAD gauntlet: the residual does not pass Silero (2026-07-12)

The barge-in consumer is a server-side VAD, not a human ear, so the
suppressor's output was judged by the actual judge: the local s2s
stack (huggingface/speech-to-speech at
`ws://localhost:8765/v1/realtime`, or that host's LAN IP if it runs on
another box) runs Silero VAD v5 with
threshold 0.6 and only confirms a speech start — and only then fires a
barge-in — after >= 384 ms of accumulated active speech
(`min_speech_ms`; chunks with prob >= threshold − 0.15 count).
`vad_gauntlet.py` replays captures through the server's own
`VADIterator` + confirmation logic (run it from the speech-to-speech checkout for
torch + the cached model). On the head-worn listening-test captures
(cold-start worst case, v80):

- **voice_on_134816 (AEC+sup on, wearer silent): ZERO episodes at every
  threshold 0.3–0.9** — the residual never even triggers Silero, let
  alone reaches confirmation. No false barge-in, including through the
  cold-start onset.
- voice_off_134816 (AEC off, control): 3 confirmed starts, 13.3 s
  active speech — constant false barge-in, as expected.
- voice_on_134924 (AEC on, wearer talking over playback): 6 confirmed
  starts at thresh 0.6 — real barge-in survives the suppressor. At 0.7+
  it starts losing genuine speech (3 at 0.7, 2 at 0.8), so do NOT
  raise the threshold as a "fix"; 0.6 is right.

So on a gap-free feed the barge-in problem is solved end-to-end at the
VAD layer. The remaining unknown is the BLE-fed duplex path (feed-gap →
resync → momentarily uncancelled echo), which `duplex_vad_test.py`
measures: a full-duplex (mic never muted) OpenAI-Realtime client for
the s2s server that streams both LC3 legs over BLE like production,
records the decoded mic stream (16 kHz wav — re-judge it with
`vad_gauntlet.py`; in duplex mode the server's `audio_start_ms` indexes
straight into the wav) plus a timestamped event log
(speech_started/stopped, response lifecycle, playback spans), and dumps
`aec('stats')` on exit — `feed_gaps`/`resyncs` are the BLE-starvation
ground truth. `--half-duplex` is the control mode, `--threshold` /
`--silence-ms` push live `turn_detection` overrides, `--pace-factor`
probes feed-starvation margin. Note the GA `audio/pcm` schema pins the
wire to 24 kHz; liblc3 resamples inside the codec (as in the SDK
examples), while the recording stays a native 16 kHz decode of the same
LC3 frames. Context for the harness: the AEC + suppressor are enabled
at boot (no `aec(true)` needed) and engage whenever the mic runs
16 kHz mono and the speaker reference is live — exactly the SDK
examples' format — so the earlier "echo picked up immediately"
observation from the flutter sample's barge-in toggle happened WITH
the AEC active — pointing at BLE feed gaps, or at the judging stack
(OpenAI's cloud server_vad has no 384 ms confirmation rule and may be
far more trigger-happy than s2s's Silero setup) — not at a disabled
canceller. This harness exists to split those causes.

## Live-duplex root cause: emission holes + ms-quantized epochs (2026-07-12)

The first real BLE-fed duplex session (`duplex_vad_test.py`, the Halo unit
head-worn, s2s, 127 s, 16 reply spans) worked mechanically — 4 real
barge-in cancels, zero resyncs, converged w — but the assistant
**self-interrupted repeatedly**: the wearer was silent 58–82 s and those
confirmed VAD starts were AEC residual. The numbers convict the live
loop, not the voice or the filter:

- Duplex residual in 300–800 Hz: −50.4 dBFS ≈ the projected uncancelled
  level (Qwen TTS is ~3 dB *quieter* on active frames than the Gemini
  bench clip) → **live in-band cancellation ≈ 0 dB**.
- Same voice, same device, quiet room, single-span bench
  (`erle_voice_test.py` with `qwen_tts_clip.wav`): per-band ERLE
  **+15.1 / +20.9 / +11.7 dB** (300–800 / 0.8–1.6k / 1.6–3.4k), and the
  bench residual scores **zero Silero episodes** in `vad_gauntlet.py`.

Host scenario 14 reproduces and explains it (see `host/README.md`,
`-DSC14_TRUE_GAPS`): every reply gap (>40 ms feed hole) wipes the
emission epoch, and re-establishment reads ms-quantized `k_uptime`
timestamps whose fractional phase is constant *within* a reply (block
period is exactly 20 ms) but arbitrary *across* replies — so each
reply's window anchor lands shifted by up to ±16 samples. ±16 samples
is ~180° of per-bin phase at 500 Hz: cancellation dies for the whole
reply, the misaligned residual reads as double-talk (p_err ≈ p_mic) so
adaptation freezes and never refits, and none of it counts as a resync.
The tick rate is 1000 Hz, so no finer kernel clock exists to fix the
timestamps.

Fixes (this branch):

1. **Driver silence-feed** (`max98357a_audio.c`): while a session is
   open (and an AEC tap is registered), a drained DMA queue is fed a
   static zero block from the completion callback instead of letting
   emission stop. The reference timeline never breaks, the emission
   epoch is established once per session (at the first real block) and
   never relocated, reply gaps and mid-reply BLE starvation become
   real silence in the reference (which they acoustically are), and
   the AEC stays warm-engaged across a whole conversation. Mic-only
   sessions (speaker session closed or never written to) still take
   the zero-latency bypass.
2. **Signed hangover age** (`audio_aec.c`): the tap ISR can stamp
   `feed_stamp_ms` between the consumer reading `now_ms` and checking
   the hangover; the unsigned age then wraps huge and fakes a
   disengage (held block dropped, sync lost, FD history wiped, no
   resync counted). Ages are now signed; "just fed" is negative.

Host matrix after the fixes: FD and TD builds all 14 checks green
(reply cycles hold 11.6–14.1 dB in every timing recipe);
`-DSC14_TRUE_GAPS` control fails tap-jitter/combo at ~5 dB as
documented.

### Device re-test (2026-07-13, the Halo unit, duplex_*_003139): partial — works, then dies at ~100 s

The silence-feed build was flashed and re-run live against s2s
(~156 s, ~20 replies, v80). Cancellation genuinely worked for the
first ~50 s of playback (suppressor "alien" texture audible, replies
mostly not self-triggering, the wearer's one real barge-in cancelled
cleanly) — then, by ear at ~100 s wall, the suppression LIFTS and the
assistant comes through near-clean; every later reply self-interrupted
at Silero. Session facts: resyncs 0, feed_gaps 1 (benign: spans the
boot-chime session end to the first duplex reply), w small — but
**`margin_last = margin_max = 7019` with `margin_min = 0`: the
window margin grew monotonically ~72 samples/s (0.45%), and
`ref_pads` ≈ 60 s.** That is the death mechanism: the reference ring
holds 256 ms; once the slip passed ~180 ms (≈ ring − window − lead),
the mic-anchored window slid off the ring's back edge, the reference
became all-pad silence, and the linear filter AND the suppressor went
inert — a slow leak crossing a cliff, not a component switching off.

What the follow-up probes pinned down (all on the Halo unit):

- `clock_rate_test.py` (first run ever on 08 — the earlier audit was
  dev-kit-only): I2S +17 ppm, mic within startup-backlog tolerance. The
  clocks are fine; 0.45% is NOT skew.
- Pure silence-feed probe (PCM session primed 1 s and left open,
  `aec('zero')`, 60 s, read clkmon ref rate): **16000.27 Hz (+17 ppm)
  — the silence-feed alone is sample-exact.**
- Therefore the over-feed appears ONLY when real BLE-fed playback
  interleaves with the silence-feed. **Suspect #1: the driver
  callback feeds the tap on every completion regardless of
  `i2s_sync_status` — an error-status completion credits 10 ms of
  reference without its emission time passing; 1 error per ~220
  blocks (0.45%) produces exactly the observed slip.** Suspect #2:
  something at real→silence→real hand-offs.

Harness caveat learned scoring this session: `duplex_ref_<ts>.wav` is
stamped at SEND time, and the client pushes 11% faster than realtime
with no back-pressure, so send-to-emission lag grows to SECONDS late
in a reply — coherence/lag searches against it need either
emission-time stamping or a multi-second search window. (This is what
made the first per-episode "wearer" classifications wrong; the wearer's ear
said self-interruptions, and the margin data agrees.)

**Resume here — next steps in order:**

1. Driver counters in `max98357a_audio.c`: silence sends, error-status
   completions, tap-FIFO mismatch drops — exposed via `aec('stats')`
   or a driver getter; plus margin/clkmon sampled MID-session (the
   post-hoc cumulative read is the known trap).
2. Local interleaved-burst probe (no BLE): lua-driven short LC3/PCM
   bursts every ~2 s over the silence-feed with the mic running;
   watch margin(t). Reproduces the slip if suspect #1/#2 is right.
3. Likely fix: skip the tap feed when completion status !=
   I2S_SYNC_STATUS_OK (count it instead); consider growing the
   reference ring 256 → 512 ms as belt-and-braces (buys ~5 more
   minutes only if the leak is fixed — headroom, not a cure).
4. Re-flash, re-run `duplex_vad_test.py`: expect margin_max to stay
   at a few hundred samples for the whole session and zero
   self-interruptions after the first reply.
5. Then: first-reply onset exposure (app-side gate or W warm-start),
   do-no-harm leak, beamformer chaining.

## Capture-loss root cause (2026-07-13): the slip was the MIC, not the tap

The 003139 session's own exit stats settle the "which side over/under-
feeds" question before any new instrumentation was needed — both
suspects in the plan above are wrong, and the counters that convict the
real culprit were in the dump's *missing keys*:

- `ref_frames/ref_us` = 1,738,219 / 108.645 s = **15999.1 Hz**: the
  speaker tap feed is kernel-clock exact to −57 ppm. Suspect #1
  (error-status completions crediting phantom reference samples) is
  **exonerated by the session's own data** — at the claimed 0.45% the
  clkmon would have read ~16072 Hz.
- `pdm_popped/mic_us` = 2,496,157 / 156.364 s = **15963.6 Hz**: the
  MIC delivered ~36 samples/s too few — **~5,670 samples (354 ms) of
  capture lost over the session**, matching the margin growth to
  within the anchor baseline (margin_max 7019). The mic wav agrees:
  155.96 s of samples across 156.36 s of contiguous activity.
- `pdm_stat_max = 8`: the 8-deep PDM hardware FIFO hit its cap. With
  `fifo-watermark = 6`, the almost-full IRQ leaves the ISR **125 µs**
  to run before overflow — easily missed under duplex BLE + LC3 load
  (and never missed idle, which is why the pure silence-feed probe and
  clock_rate_test measured sample-exact).
- `pdm_dropped` / `pdm_overflows` existed but never made the dump: the
  stats one-liner printed the whole table in ONE print, which
  overflows the print buffer and drops arbitrary keys. Fixed
  (batched prints everywhere; see below).

Why the AEC dies from this: the reference window is anchored by
**cumulative seen mic samples** (`mic_total`), and so are BOTH operands
of the drift check (`desired_end` vs `r + n`) — so upstream losses
retard the window at exactly the loss rate while drift reads 0. Margin
= `w − (r+n)` grows monotonically (write head w is real-time-clocked),
the window slides off the 256 ms ring's back edge, the reference goes
all-pad, and the linear filter AND suppressor die with **zero resyncs
counted**. Host check 15's `-DSC15_NO_LOSS_HANDLING` control reproduces
the exact signature (margin +320/drop, resyncs stuck, ERLE → 0).

Loss layers found in `t5838_alif_pdm.c`:

1. **HW FIFO overflow handler cleared and reconfigured the FIFO** —
   throwing away the 8 good samples it held, plus restart time, all
   UNCOUNTED (not in popped, not in dropped). The upstream Zephyr
   alif_pdm driver never does this; the overflow itself only loses the
   sample(s) the decimator couldn't push.
2. Queue-full / slab-alloc drops — counted in `t5838_diag_dropped`,
   but invisible to the AEC (and, per the print bug, to the dump).
3. The intentional start-of-stream discard was counted in the same
   `dropped` counter — split out (`pdm_discarded`) so drops mean loss.

Fixes (this commit set):

- **Driver: drain-on-overflow** — ack the error IRQ, count the event,
  fall through to the normal FIFO drain. No clear, no reconfig.
- **`fifo-watermark` 6 → 4** (halo.dts): 250 µs of ISR headroom for a
  ~1.3× ISR rate. (PKDET interval decoupled from the watermark so
  audio-detect behaviour is unchanged.)
- **Exact loss accounting**: the mic thread reports the driver's
  `t5838_diag_dropped` delta to `audio_aec_note_mic_loss()` before each
  block; the AEC advances `mic_total` (the timeline stays honest), the
  window jump re-anchors through the normal drift check as a counted
  resync, and W — kept across resyncs — cancels again immediately.
- **Late-floor backstop** for losses nobody can count (a HW overflow's
  true cost is unknowable): capture-epoch observations run persistently
  LATE when samples vanish, while genuine consumer backlog returns to
  its floor within seconds. A two-bucket windowed MIN of the lateness
  (4 s window) above `AEC_LATE_FLOOR_MS` (4 ms) slides the epoch later
  by the floor and reopens the refinement freeze; the drift check then
  re-anchors. `cap_slips`/`cap_slip_ms`/`cap_late_floor` in stats.
- **Speaker TX diag counters** (`max98357a_audio.c`): real/silence
  sends, error-status completions, tap-FIFO drops, cb send failures —
  `spk_*` in `aec('stats')` — so the next timeline dispute is settled
  by counters, not theory.
- **Mid-session stats sampling**: `lua/duplex_frame_app.lua` (an
  instrumented copy of the SDK frame app) answers `STATS_MSG` (0x32)
  with batched `#stats` prints while the app keeps running;
  `duplex_vad_test.py` samples every 10 s (`--stats-interval`) into the
  events jsonl — margin(t) and the loss ledger as a time series, not a
  post-hoc cumulative read.
- **`interleave_probe.py`**: no-s2s load repro/validation — speaker
  session open (silence-feed), 300 ms tone burst every ~2 s through the
  real write path, LC3 mic streamed over BLE, stats sampled per 2 s
  segment. Verdict line: margin slope. On a fixed build margin stays
  bounded regardless of what `pdm_dropped`/`pdm_overflows` do.

Host harness: check 15 (reported-loss cycles + unreported-loss burst)
green in both builds — FD worst-cycle 11.1 dB with 16 resyncs for 15
drops and margin pinned at baseline; late-floor slides +88 ms and
recovers 12.1 dB. Controls: `-DSC15_NO_LOSS_HANDLING
-DAEC_LATE_FLOOR_MS=1000000` fails both parts with the device's death
signature; `-DSC14_TRUE_GAPS` still fails tap-jitter/combo as
documented.

The ring stays at 256 ms: with the timeline honest, margin is bounded
by design and the 8 KB is better spent elsewhere.

**Validation plan** (flashing the Halo unit pre-approved once the dev kit
build boots): host suite green both builds (done) → build + flash the dev kit →
`interleave_probe.py` on the dev kit (margin flat, counters ticking sanely) →
flash 08 → `interleave_probe.py` on 08 → `duplex_vad_test.py` vs s2s
on 08 (on-desk; margin_last flat all session, zero late-session
self-interruption cascade). Head-worn re-test when a wearer is available.

### Dev-kit probe round 1: the loss is INVISIBLE at the counters (2026-07-13)

First dev-kit run of `interleave_probe.py` (36 s, post-fix build): production
itself ran **0.2% slow** (pop rate 15967 Hz at the PDM ISR, i.e.
producer-side, not consumer backlog) with `pdm_dropped=0`,
`pdm_overflows=0` — and `pdm_stat_max` pinned at 8 every segment even at
watermark 4. Root cause: the t5838 fork only ever enabled
`PDM_FIFO_ALMOST_FULL_IRQ` — **the overflow interrupt/status was never
enabled**, so hardware drops at FIFO-full are completely silent (the
upstream alif_pdm driver enables both). Meanwhile the **late-floor
backstop did its job**: 8 slides totalling +58 ms ≈ the true deficit,
margin bounded (wander ±300 around anchor, no runaway), 4 resyncs — the
system survives what it cannot count, but a resync every ~9 s is churn
worth eliminating at the source.

Round 2 (enable `PDM_FIFO_OVERFLOW_IRQ`, PDM IRQ priority 4 → 2)
**wedged the dev kit at the first loaded segment**, and priority was
exonerated by bisect (priority reverted, still wedged). The real bug is
a register-map error as old as the driver: **overflow STATUS is bit 0
of `PDM_ERROR_IRQ` (0x10), edge-triggered STICKY, cleared only by
reading that register** (B1 HWRM 14.6.5.3.5) — the ISR tested bit 1 of
`PDM_WARN_IRQ`, which doesn't exist, so the old clear-and-restart
overflow branch was DEAD CODE forever (that's why `pdm_overflows` read
0 even in the lossy 003139 session), and once the enable bit was set,
the first FIFO-full latched an ERROR status nobody ever read → the
OR'ed LPPDM_IRQ line stayed asserted → ISR storm → device wedge. Fix:
the ISR reads (= acks) `PDM_ERROR_IRQ` at entry, counts overflows from
its bit 0, and falls through to the normal drain.

Round 4 (dev kit, 72 s probe): no wedge, and the loss is finally on the
books — **732 overflow events (~10/s)** under burst+mic load at
watermark 4 and priority 4, `pdm_dropped` still 0. Margin bounded
(±600 wander, VERDICT bounded) with 22 slips + 9 resyncs absorbing it —
the safety net works, but that churn is a resync every ~8 s. Round 5:
PDM IRQ priority 4 → 2 (bisect-exonerated for the wedge), plus the
late-floor slides by `floor − threshold/2` instead of the full floor
(full-floor slides bake the min-backlog observation noise in every
time — measured as a slow negative margin drift that epoch refines
kept pulling back).

Round 5 result: **priority changed nothing** (682 overflows/72 s vs
732) — and the arithmetic says why, twice over. A drain empties the
FIFO, so an overflow needs an entry-to-entry gap > 8 samples ≈ 500 µs
*regardless of watermark* (which is also why 6 → 4 changed nothing:
the watermark moves the IRQ, not the fill-to-overflow time from
empty). And a >500 µs blackout that ignores NVIC priority means the
blocker is `irq_lock` sections or zero-latency/priority-0 contenders
(BLE controller service on this core is the prime suspect) — no
priority assignment escapes those. Round 6 adds an ISR entry-gap
tracker (`pdm_gap_max_us`, `pdm_gap_over` — the latter should track
`pdm_overflows` 1:1) to size the blackouts. **The definitive fix is
DMA-draining the LPPDM FIFO (`LPPDM_DMA_REQ` exists per the B1 HWRM) —
hardware-timed, immune to CPU latency — queued as follow-up work.**
Meanwhile the accounting stack keeps total alignment error to a ~4-6 ms
sawtooth (slides every ~3 s), which the FDAF tracks the same way it
tracked the first 50 s of the 003139 session — degraded vs bench, but
functional and, critically, STABLE for arbitrarily long sessions.

### Unattended duplex re-test (Halo unit, on-desk, 2026-07-13, 5 min)

`duplex_vad_test.py --auto-prompt` (new: text-driven replies, so the
loop runs with nobody in the room — every confirmed `speech_started`
is then an AEC-residual false barge-in by definition). Result, 10
replies over 300 s at v80:

- **`margin_max` pinned at 763 the whole session** (old build: monotone
  to 7019 and the ring cliff at ~100 s). The death mechanism is gone.
- **ZERO `speech_started` events in 5 minutes** — not one false
  barge-in, 3× past the old failure point. (On-desk coupling is weaker
  than head-worn; the head-worn re-test is still the real exam.)
- Remaining defect, visible in the margin(t) series: under duplex load
  the backlog floor is noisy (11-28 ms observations vs 2-11 in the
  probe) and the late-floor **over-slid** — margin drifted negative
  (−1100 by t=290, tail pads ~37% late-session, i.e. cancellation
  degrading in slow motion with refines fighting back).

Round 7 closes it: overflows are now countable AND sizeable, so the
driver estimates the per-event loss from the ISR entry gap (everything
past the FIFO's 8-frame capacity, `pdm_lost_est`) and the mic thread
feeds it through `audio_aec_note_mic_loss` — the EXACT path, no
inference. The late-floor demotes to a true catastrophe backstop
(`AEC_LATE_FLOOR_MS` 4 → 12): sized losses never reach it.

Round 7 re-run (5 min, same protocol): still zero `speech_started`;
`cap_slips` down to 7 and the mic ledger is EXACT — accounted mic rate
16000.16 Hz (+10 ppm), the gap-based overflow estimate matching the
true deficit to <1%. But the margin still drifted −4.4/s, and the
final clkmon pins it on the OTHER side: **`ref_frames/ref_us` =
15994.8 Hz — the reference tap runs ~5 samples/s slow.** Same >500 µs
blackouts, different victim: when the I2S TX FIFO runs dry the
hardware stretches emission with NO status raised
(`spk_err_completions` 0), while the tap credits a full block per
completion — an unflagged emit-side timeline slip that reaches the
ring's far edge in ~30 min. Round 8: an **emit-side late-floor twin**
(`AEC_EMIT_FLOOR_MS` 4 ms — producer observations are ISR-stamped, no
consumer-backlog noise, so the tight threshold is safe) slides
`emit_epoch` by the windowed-min lateness; `emit_slips`/`emit_slip_ms`
in stats.

**Round 8 final validation (Halo unit, 5-min unattended duplex,
duplex_events_033020): CLOSED.** `margin_last` bounded **[−37, +667]
for the whole session with no drift** (round 7: −4.4/s; round 6:
late-floor churn; original: monotone death at ~100 s). `ref_pads` 222
TOTAL (vs 2.9M in round 7 — the window served real reference
essentially 100% of the time). Accounted mic timeline 16000.23 Hz;
emit floor absorbed the I2S stretch with 24 slips / +76 ms ≈ exactly
the measured 5 samples/s. **Zero `speech_started` in 5 minutes / 10
replies — not one false barge-in.** Residual churn: 121 resyncs
(~1/2.5 s, each a cheap ref-history refill with W kept and alignment
correct throughout) — proportional to the overflow rate, and goes away
with the LPPDM DMA follow-up. Caveats for the head-worn re-test: on-desk
coupling is too weak to trip Silero even uncancelled, so this run
validates the TIMELINE BOOKKEEPING, not cancellation depth — the wearer's ear
plus a head-worn margin(t) series is the remaining exam.

## Head-worn duplex re-test: the barge-in gap is ONSET, not depth or churn (2026-07-13)

The remaining exam, run head-worn on the Halo unit against s2s (round-8
build, `duplex_vad_test.py`, v80). Four runs, and they converge on a
single, initially-surprising conclusion: **the head-worn false-barge-in
problem is a cold-start/transition convergence effect — a converged
filter holds Silero-silent through 29 s of continuous playback, right
across the ~2 resync/s overflow churn.** The churn (the LPPDM DMA
follow-up's target) is real but is NOT the barge-in driver, and the
earlier suspicion that it was is retracted.

**1. Interrupt mode (server auto-interrupt on): the cascade the wearer heard.**
98 confirmed `speech_started` in ~250 s — every reply killed 1–2 words
in (`response_done status=cancelled` ~0.9 s after `playback_start`),
then the next reply cold-starts and dies the same way. `w_norm2=0.021`:
the filter never converges because no reply survives long enough to let
it. Self-sustaining deadlock. Timeline was fine throughout (margin
bounded `[−69,+667]`), so this is not the round-8 ring-slip death.

**2. Silence control (single 0.5 s reply, then 72 s idle).** Zero
`speech_started` in the idle stretch, and `resyncs` flat (223→224),
`pdm_overflows` ~2/s. Two facts fall out: the false barge-ins are driven
by **playback echo residual** (not injected/ambient noise in silence —
rules out a do-no-harm-into-silence cause here), and **the overflow/
resync churn is driven by the duplex playback load** (idle mic-only is
nearly clean; sustained head-worn playback runs ~140 overflow/s, ~2.3 %
mic loss, ~2 resync/s — far above the dev kit probe's ~10/s).

**3. No-interrupt, short back-to-back replies (auto-interval 4).**
Replies complete, but `speech_started` fires every 1–3 s throughout —
because every reply is short, so the stream is *all* onset/offset
transient and never reaches steady state. This is the same signal as run
1 with the killing disabled.

**4. The decisive run — 2 s onset guard, a 31 s continuous reply.** A new
`--onset-guard-ms` (see below) let one reply run 31 s uninterrupted
(18.1→49.5 s of playback). `speech_started` fired at **18.7 s (0.6 s
into playback — onset) and 47.8/49.2 s (the tail/drain)** and **nothing
in the 29 s between.** A converged filter head-worn is dead silent to
Silero for 29 s straight — matching the bench `erle_voice_test` +15.6 dB
/ zero-episode result — *through* the same ~2 resync/s churn. The false
barge-ins live at reply **onsets and ends/transitions**, not in the
converged middle. Across the guard run, 7/7 replies completed, 0
cancellations; the wearer's ear: "mostly whole sentences, self-interrupted
only once" (was: constant).

**Conclusion.** The barge-in blocker is the **onset stage (next-step #3),
not the LPPDM churn (#2)** — #2 is demoted to robustness/efficiency. The
original cascade is just onset-transient stacking: short replies ⇒ every
reply is a fresh cold start ⇒ perpetual onset residual ⇒ constant
self-interruption. Give a reply room to converge and it goes quiet. The
timeline held head-worn all session (margin bounded `[−85,+667]`) —
round-8 fix confirmed on 08 head-worn.

**Harness change (`--onset-guard-ms`, committed).** Client-side barge-in:
disables the server's auto-interrupt and cancels a reply from the client
only after it has played `N` ms (flushing the local speaker queue is what
actually stops the echo, regardless of whether s2s honours
`response.cancel`). It broke the cascade (replies completed), but **could
not be measured cleanly, and disabled real barge-in**, because of the
harness's fast-push: the client pushes audio ~11 % fast with no
back-pressure, so the server marks a reply "done" (`response_active`
false) ~1.8 s in while the device keeps *emitting* it for ~8 s. With
barge-in gated on `response_active`, once generation finishes no cancel
fires — real or false (`0 client_barge_in`, `0 barge_guarded`; the guard
never even engaged — what broke the cascade was simply disabling the
trigger-happy server auto-interrupt). The wearer confirmed they couldn't barge in
on a long stretch. **A faithful app-side barge-in grace window needs a
realtime-paced harness (or the flutter app), where `response_active`
tracks actual playback.**

**Next:** (1) firmware onset cancellation for the first ~1 s of each
(re)engaged reply — the residual suppressor acts instantly with no
convergence, so an onset-boosted suppression schedule (and/or holding its
smoothed spectra warm across the speaker-idle bypass) should crush the
onset residual below Silero; a WARM re-engage currently skips the entire
cold-onset schedule by construction, yet every head-worn reply onset
still trips VAD, so the boost must apply on re-engage too. (2) App-side
barge-in grace window as the pragmatic backstop (steady-state is already
clean). (3) LPPDM DMA (#2) for churn/efficiency — no longer on the
barge-in critical path. (4) beamformer chaining.

## Onset duck: cold-start cascade fixed; evaporation-burst cascade remains (2026-07-13)

The onset fix, landed and validated head-worn on the Halo unit over four
on-device iterations (`audio_aec.c`, FDAF build; all params
`-D`-overridable for host A/B). Then a realtime-paced test harness
exposed the *next* problem cleanly. Net: the cold-start cascade is
gone and the uplink echo is "almost all suppressed" by ear, but the
duplex loop is not yet stable — a churn-driven suppression *evaporation
burst* still slips one false barge-in through, and s2s then answers
its own echo into a self-sustaining cascade.

### The onset duck (fixed)

A per-reply suppression duck: for a window after each reply onset the
residual suppressor runs hard, then eases to steady. It needs no
convergence of its own, so it covers the exact gap the linear filter
can't. Two design points, each cost an iteration:

1. **Trigger = the reference gate's rising edge** (silence→active
   reference), NOT the speaker-idle re-engage. The round-8 driver
   silence-feed keeps the speaker session engaged across reply gaps, so
   a speaker-idle re-engage fires only ONCE per session — the first
   attempt armed the duck there and never re-armed per reply (device:
   identical cascade, no effect). The gate opening is the true
   per-reply onset.
2. **A prediction-independent blanket ceiling**, not just a
   suppressor-ratio boost. the wearer's mic recording was the tell: the echo
   "quieted but had no alien/suppressed texture", i.e. the ratio gains
   `1 − β·Sy/Se` barely move because the linear filter under-predicts
   the onset echo. A hard in-band gain ceiling eased from `ONSET_GCAP`
   up to 1.0 — a spectrally-flat duck through the alias-free kernel (no
   musical noise) — crushes the onset regardless of prediction.

Dose-response on device (interrupt mode, auto-prompt, wearer silent;
completed/cancelled replies over ~100 s):

    no fix:                     ~1 / 98   every reply killed 1-2 words in
    ceiling off (ratio only):     3 / 28
    1s ceiling, -20dB:            -        by ear: kills moved 1s -> ~2s
    2.5s hold + 1s ease, -20dB:   7 / 5    cascade broken; a couple of
                                           onset starts still self-interrupt
    2.5s hold, -34dB:            10 / 7    onset starts muted

The 1s→2s shift proved the mechanism (longer duck = later survival);
death at 2s was the duck fading before the FIR converged (the guard-run
measured convergence at ~1-2s once a reply is NOT killed), so the window
now HOLDS full strength for `ONSET_HOLD_HOPS` (~2.5s) then eases.
Deepening the ceiling −20→−34 dB muted the last onset leaks (at −20 dB
the onset voice was "partly audible", enough to trip Silero
occasionally). Params: `AEC_SUP_ONSET_HOPS` 175 (~3.5s total),
`_HOLD_HOPS` 125 (~2.5s full), `_GCAP` 0.02 (−34 dB), plus the secondary
`_BETA`/`_FLOOR` ratio boost. `ONSET_HOPS=0` or `ONSET_GCAP=1.0`
disables it. Host: cold-onset 7→16.8 dB, **double-talk preserved**
(−1.5 dB), all checks green both builds. **Head-worn verdict:** replies
survive and play whole paragraphs, real mid-reply barge-in is heard and
answered (the duck only covers the onset), and the mic recording is
ducked ~3s per utterance then reverts, re-arming each utterance. The
duck attenuates the wearer's uplink too, so barge-in in the first ~2.5s
is suppressed — a deliberate firmware barge-in grace window, no app
change needed.

### What the realtime-paced harness revealed (the next problem)

The rig's default fast-push (stream a whole reply onto the device far
ahead of realtime) confounds barge-in scoring: the device buffers 12+ s
and its drain tail trips the VAD on later replies (a cancel flushes
`frames=1286` of unplayed audio). A wall-clock pacing mode was tried
(stream at realtime, ≤0.3 s buffered) — it de-confounded the *device*
timeline and gave the clean read below, but **starved the device buffer
and made playback bursty/choppy** (extra transients the AEC can't
cancel), a net regression. The send-ahead is intentional for smooth
playback; the pacing change was reverted. A future realtime mode needs a
~1.5 s buffer (smooth) rather than 0.3 s. Even so, the paced run's
findings stand:

- **The uplink echo is almost fully suppressed by ear** for most of a
  reply — the onset duck + suppressor are doing their job.
- **Isolated replies mostly complete**, with clean multi-second quiet
  gaps after a reply (a completed reply's echo does NOT trip the VAD —
  the converged filter holds).
- **The failure is a suppression *evaporation burst*.** By ear the
  suppression "basically evaporates" for a moment (~43 s, ~62 s in the
  paced run); the stats show continuous churn behind it — `resyncs`
  climbing ~0.4/s, `pdm_overflows` ~25/s, `margin_last` dipping to
  91/123. **Each resync wipes the reference history and disrupts the
  suppressor's echo prediction (Sy), so the gains snap to 1 and the
  echo passes uncancelled for that burst** — one false barge-in.
- **Then s2s answers its own echo.** A confirmed false
  `speech_started` is treated as wearer speech, so the server generates
  a *reply to the echo*, whose onset trips the VAD, and it
  self-sustains — "started well, then once an interruption happened it
  looped every ~3 s" (first-healthy-then-cascade; realtime run 6
  completed / 15 cancelled, but ~5/2 in the first 60 s then a cascade).

So the onset duck is not the whole barge-in story after all: it fixed
the *cold-start* cascade, but a *churn-driven* evaporation burst is a
second, independent trigger — which re-promotes the LPPDM DMA churn
work, and adds a system-level phantom-VAD loop that is partly the
server's (s2s responds to a no-transcript VAD event).

### Next (fresh session)

1. **Kill the evaporation bursts.** Two angles: (a) re-arm the onset
   duck on a RESYNC (a resync is the same "prediction not established"
   condition as an onset — arming the duck for ~0.5-1 s should cover the
   burst; cheap, directly analogous to the onset trigger); (b) the
   **LPPDM DMA FIFO drain** to cut the overflow→resync rate at the
   source (hardware-timed, immune to the >500 µs CPU blackouts) — now
   back on the barge-in critical path, not just efficiency.
2. **Break the phantom-VAD loop** (system-level): don't let s2s
   answer a VAD event with no ASR transcript, and/or an app-side
   barge-in debounce (ignore a barge-in that isn't followed by real
   decoded speech).
3. **Harness:** a realtime pacing mode with a ~1.5 s buffer (smooth
   playback + de-confounded scoring), so barge-in can be measured
   without the fast-push drain-tail artifact.
4. Then the residual suppressor tuning / beamformer chaining.

## LPPDM DMA drain lands: churn eliminated at the source (2026-07-14)

Both angles of "kill the evaporation bursts" are now in (commits `a0a7cff`
resync-rearm duck, `034cc3e` DMA drain), and the **DMA drain is the real
win** — it removes the resync churn at the source, so the resync-rearm duck
(and its prediction-blind near-end damage) becomes unnecessary.

### The resync-rearm duck (1a) — landed, then superseded

Re-arming the onset duck's prediction-independent blanket ceiling on each
genuine resync (`AEC_SUP_RESYNC_HOPS`, ~1 s, MAX so it never shortens a
reply's own onset) **broke the cascade**: worn Halo unit, 26 completed / 2
cancelled, stable over 8 min under continuous churn (both cancellations in
the first minute). But it is prediction-blind, so during churn it ducked the
wearer's own near-end voice and barge-in too — the wearer heard short interrupt
phrases ("stop") fail and near-end voice get butchered (~20 s, ~47 s). A
stop-gap, not the answer. With the DMA drain cutting resyncs to ~1 per 90 s,
set `AEC_SUP_RESYNC_HOPS=0` to disable it (next session).

### The LPPDM DMA drain (1b) — the structural fix

The 8-set PDM FIFO gave the almost-full ISR only ~250 µs to overflow; the
>500 µs duplex blackouts overflowed it ~25/s, and each loss walked the
capture timeline until a resync. Route the FIFO watermark to a hardware DMA
handshake so **dma2 (PL330) drains it, immune to the blackouts** — the CPU
leaves the FIFO-drain loop entirely (`pdm_isrs` 0). Wiring facts came from
the B1 HWRM (absent from the tree and the Alif HAL): **LPPDM DMA request =
`DMA2_REQ[30]`, Group 2** (Table 14-95); **PDM_CTL1 bit 24 `USE_DMA`** routes
the watermark; `dma2` bumped 4→5 channels, `pdm_audio` gets
`dmas = <&dma2 4 30>`. The driver mirrors `i2s_sync`'s RX path
(PERIPHERAL_TO_MEMORY, ping-pong), gated on the `rxdma` DMA property (drop it
to fall back to the ISR). Suppressor onset/residual stages are unchanged.

**Four bring-up bugs, each with an audible signature** (all fixed):

1. **16-bit sub-word reads of the 32-bit-only FIFO APB** → misaligned white-
   noise garbage (the "Apollo 13" mic the wearer first heard). Fix: read `CH2_CH3`
   at native 32-bit width into a scratch buffer; mono keeps the low 16 bits
   (ch2), stereo words ARE the `[ch2,ch3]` set.
2. **Skipped start-of-stream discard** → capture-timeline offset, `ref_pads`
   grew. Fix: drop whole settling DMA transfers (count `pdm_discarded`).
3. **Wrong handshake type** — the HAL `dma_event_router_configure()` has a
   dead line that always clears the ACK-type bit. Fix: an `i2s_sync`-style
   configure that SETS it (level handshake).
4. **Burst too large** — PERIPHERAL_TO_MEMORY forces PL330 burst-request mode
   (`breq_only`), but LPPDM signals "≥1 sample available" (single semantics);
   `burst == watermark` over-read **~2× → the mic delivered at double rate**
   (the wearer: WAV played back time-stretched / half-speed; `margin` raced to
   −1.5 M). Fix: `source_burst_length = 1`.

**Validation.** the dev kit `clock_rate_test`: mic-only **16000.00 Hz +0 ppm**,
`overflows 0 isrs 0` in every phase including duplex (was ~25/s); raw mono
capture 0.14 % clip (clean). The Halo unit worn duplex: **`margin` bounded**
(±~170, was racing), **`pdm_overflows 0`, `resyncs ~1` per 90 s** (was ~36),
**`w_norm2` nonzero → the filter converges = real cancellation** (was inert
0.0 on every broken DMA run), and by ear the assistant echo is audibly
**suppressed at the correct sample rate and low level** — the whole point.

### The next problem (revealed by the clean run)

Worn WAV (`duplex_mic_235040.wav`): for the first **~30 s the assistant voice
is clearly suppressed; at ~31 s the suppression abruptly lifts** — the voice
stays quiet and undistorted but becomes clearly understandable. Working
hypothesis: the low-but-audible post-31 s residual is enough for the server
VAD to latch where the harder-ducked 0–30 s residual was not — i.e. a
suppressor-state or filter transition at ~31 s, not a capture problem
(sample rate and cleanliness are correct throughout).

### Next (fresh session)

1. **Chase the ~31 s suppression lift.** Correlate the WAV timestamp against
   `aec('stats')` (a resync? onset-duck expiry? `sup_gmin` / `w_norm2`
   transition?) — instrument what changes at ~31 s. This is now the barge-in
   gate, not the churn.
2. **Disable the resync-rearm duck** (`AEC_SUP_RESYNC_HOPS=0`) now that
   resyncs are rare, and re-test worn: near-end voice and barge-in
   responsiveness should return with no cascade.
3. **`ref_pads` minor growth** — a bounded burst around 60–80 s worn (to
   ~225 k, then it slowed and `margin` recovered); confirm it's a transient
   reference-feed gap, not a slow leak.
4. **Polish the DMA path**: double-buffer the ping-pong (remove the
   extract+re-arm gap), decide selectable-vs-replace vs the ISR drain, and
   consider a true SINGLE-request DMA (needs the PL330 driver to not force
   `breq_only` for this slot).
5. Still open from before: the phantom-VAD loop (system-level, s2s) and
   the harness realtime pacing mode.

## FDAF telemetry + the 63s barge is reference misalignment, not depth (2026-07-15)

Added live FDAF telemetry, then used it to overturn the "31s knee" framing:
the knee was a **volume-60 starvation artifact**, and at the validated vol 80
the real remaining barge-in gate is a **reference-window misalignment** that is
energy-quiet (inaudible, same ERLE) but structure-preserving, so the server VAD
fires on it. Tip: feat/aec (telemetry commit pending).

### Live FDAF telemetry (w_norm2 was blind by construction)

`w_norm2 = Σ aec.w[k]²` reads the **time-domain shadow taps**, which the FDAF
path only refreshes round-robin (one partition/block) for `aec('dump')` - so on
short or low-SNR runs it reads ~0 *while the frequency-domain filter is
converging*. That is why every worn run showed `w_norm2=0`; the README's
"w_norm2 nonzero = convergence" was a bench run where the shadow happened to be
populated. Likewise `sup_gmin=1.0` only means the suppressor saw ~no predicted
echo (`p_ref≈0`), not that it is off.

New fields, plumbed `audio_aec_snapshot()` -> `lua_microphone.c` aec('stats')
-> the frame app's `#stats` (auto-included, it iterates `pairs()`):
- **`fd_wnorm2`** - Σ|W|² over ALL FDAF partitions (`fd.W[FD_K][FD_N]`),
  computed at snapshot time so it tracks convergence *immediately*. The true
  live convergence measure; rises to ~100+ on a converged worn vol-80 run.
- **`sup_sy` / `sup_se`** - total predicted-echo / residual power at the
  suppressor (~0 sy => nothing to suppress, gmin stays 1.0).
- **`sup_gmean`** - mean suppressor gain (gmin is the single deepest bin).
- **`sup_onset`** - onset/resync duck hops remaining (counts 175->0 per reply).

Host FDAF suite still 0 failures; both devices flashed/booted/self-confirmed.

### The 31s knee was volume-60 reference starvation

The `duplex_mic_235040.wav` "distorted 0-30s, clears at 31s" was recorded at
**volume 60**. The new telemetry shows why it was inert: `p_ref=0`,
`fd_wnorm2=0`, `sup_gmin=1.0` the whole run - at vol 60 worn, the echo coupling
never reached the adaptation gate, so the FDAF filter never converged and the
residual suppressor thrashed low-SNR musical noise for ~30s. The "knee" was that
thrashing finally settling; it did **not** reproduce on the other worn run
(234118, louder/cleaner from the start), i.e. data-dependent, not a schedule.

At **vol 80** worn (`duplex_mic_004647.wav`) it is a non-issue: `p_ref` 85-1700,
`fd_wnorm2` 0->19 in ~10s (->106), voice-band residual flatness ~0.02 by t=4s.
The only distortion is the **per-reply onset duck** (`AEC_SUP_ONSET_HOPS=175` ~
3.5s, re-armed on each reply's reference rising edge) - each utterance starts
suppressed for ~3-5s then clears, by design, exactly as heard. **Test worn AEC
at vol 80**; vol 60 starves the reference. (Old "next" item 1 resolved; item 2,
`AEC_SUP_RESYNC_HOPS=0`, is moot here - `resyncs` only reached 2.)

### The 63s self-interruptions: reference misalignment, energy-quiet, structure-preserving

Run 004647 (vol 80, worn, auto-prompt) was clean and **barge-in-free for its
first ~63s**, then fired **11 false barge-ins in a tight 65-85s cluster** (a
cascade: each barge cancels a reply, the re-reply is barged at its onset). Three
independent views disagreed until Silero settled it:

- **Onset residual (RMS/ERLE):** the barging replies are **not** worse -
  reply@51.6s (no barge) and reply@63.5s (barge) both leak ~-29 dB. So it is
  *not* louder echo, and it sounds identical by ear (the extra leak that trips
  the VAD hides under the normal onset-duck distortion).
- **Telemetry:** at mic_s 63.9 the reference-window `margin` steps **+91 ->
  -69** and never recovers, `ref_underruns` climbs +53/s, `ref_pads` +3657/s,
  `p_err` spikes ~1000x. The reference feed starved and the window went
  misaligned. (This answers the prior item-3 question: the 60-80s `ref_pads`
  growth is a **persistent slip, not a transient** - margin ends at -197.)
- **Silero replay on the mic WAV** (thresh 0.6, the server's own detector):
  speech prob **~0.00 for 0-62s, then 0.93+ for 63-85s** - a hard, sharp
  transition at exactly 63s, per-reply onset peak 0.00-0.17 (clean) vs 0.97-1.00
  (barged).

Mechanism that ties all three together: a **misaligned** reference makes the
filter subtract a *time-shifted* echo. That cancels the echo's **energy** to ~
the same level (so ERLE/RMS and the ear see no change) but no longer removes the
assistant's **speech structure** (two overlapping speech copies are still
speech) - and **Silero fires on structure, not level** (prob 0.00 -> 0.93).
Conversely, while the reference is *aligned* (0-62s) the cancellation strips the
speech structure and Silero sees ~0 even though the residual is audible. So the
barge-in gate at vol 80 is the reference feed/margin, **not** the suppressor and
**not** cancellation depth.

### Next

1. **Harness vs device on the margin collapse.** Is the ~63s starvation the BLE
   speaker feed pacing (`pace_factor` 0.9, the harness) or a device-side
   re-anchor that should have recovered the window and didn't? Re-run vol 80
   with different pacing / a single long reply to isolate. This is now the real
   barge-in gate.
2. **AEC-off vol-80 worn baseline** (proposed control): an `--aec-off` harness
   flag (`frame.microphone.aec(false)`; frame app uploads fresh, no reflash) to
   confirm the low echo is the *filter* not worn acoustic bleed - expect Silero
   to fire continuously on the raw echo with AEC off, holding volume at 80.
3. Consider a suppressor that keys on residual **speech-structure** (not just
   predicted-echo power) so a misaligned-but-quiet residual is still flattened
   below the VAD.

## Prediction is exhausted; pivot to the suppressor + envelope gate (2026-07-15)

Chased "shrink the VAD-tripping residual by predicting the echo better" (linear
convergence, then a nonlinear echo model) to a **clear negative**: the voice
echo is not stably predictable, so no better predictor helps. The real
cancellation is non-predictive (adaptive tracking + the residual suppressor),
and the barge-in lever is the suppressor's blanket ceiling - which must be
**sustained past the onset window and released only for genuine near-end**, by
a signal that is NOT fooled by room noise. Firmware tip unchanged; the failed
option-A DTD gate and the sustain mechanism are left in as flags, default off.

### Offline feasibility harness (new, reusable)

- **`host/aec_wav.c`** - runs a real (mic, ref) WAV pair through the ACTUAL
  firmware `audio_aec.c` (FDAF) and writes the AEC output, so suppressor
  changes are A/B'd against real worn echo with no device. Feeds the reference
  only for non-silent blocks (the harness pads inter-reply gaps with exact
  zeros) so the speaker-idle gate works - feeding the zero padding kept it
  "active" forever and put `g_cap` in the wrong places (a real bug, fixed).
- **`silero_score.py`** - scores a WAV with the server's own VAD (Silero v5,
  thresh 0.6); run with the s2s venv. The barge-in oracle: sec>=0.6 = false
  self-interruptions. Raw echo ~66%, current-fw AEC output ~3%, a working
  sustained scramble ~0%.
- **`make_suppressor_cases.py`** - builds three eval mics from the AEC-off worn
  vol-80 capture (`duplex_mic_011535` raw echo + `duplex_ref_011535`):
  `case_echo` (want Silero ~0%), `case_nearend` (a near-end burst at 76-82s,
  want it PRESERVED), `case_noise` (continuous background noise, want Silero
  ~0% - a good gate must NOT release on noise).

Batch nonlinear-regression scripts and the FDAF-mirror convergence probe live
in the session scratchpad; the three files above are the durable harness.

### Why prediction is a dead end (the evidence)

- **Nonlinear (2nd-order `x^2`, `x|x|`): no out-of-sample benefit.** In-sample
  gains (+4.8 dB in-band on 32 voice pairs) were pure overfitting. Train/test on
  60 noise pairs: flat. Cross-set CV on **5x32 fresh worn voice pairs** (held
  out one set): 2nd-order identical to linear on every fold. `x^3` was already
  dead in Exp 1.
- **Linear convergence: no gap.** The FDAF (+4-5 dB in-band on voice,
  `offline_perbin.py`) already sits AT the coherence LTI ceiling (+3.4 dB, mean
  mic<->ref coherence only ~0.40). Nothing to close.
- **Linear as a FIXED/calibrated model on voice: ~0-2 dB held-out** (cross-set
  CV, L=1024 covering the ~875-tap / ~55 ms echo IR). Device-aligned NOISE
  pairs give 25 dB held-out; VOICE gives ~0-2 dB. So the voice echo is
  fundamentally low-coherence / not stably predictable across realizations
  (per-pair lag also smears 490-886 taps within a capture). The "fixed geometry
  -> calibratable" hope does not hold for the voice echo.
- **What works is non-predictive:** online tracking (~5 dB, chases the moving
  alignment) + the residual suppressor (+5 -> +12-14 dB). This also corrects the
  earlier "linear bounded ~13-16 dB by harmonic distortion" note: harmonic/
  nonlinear echo is >25 dB down (negligible); the 13-16 dB is a colored-voice
  convergence/coherence limit, not a distortion floor.

### The suppressor scramble, and why the gate is hard

The suppressor's **blanket ceiling `g_cap`** (prediction-INDEPENDENT, ~-34 dB on
every in-band bin, alias-free kernel) is what scrambles the echo's speech
STRUCTURE below the VAD. It is armed only for the onset window (~3.5 s) then
eases to 1.0 (off) - which is why each reply "clears up" after 3-5 s and the
clean linear residual can re-trip Silero mid-reply (esp. once alignment drifts,
the 63 s barge).

Offline A/B on the three cases (`aec_wav` + Silero), current baseline:

| variant                         | echo | noise | near-end RMS |
| ------------------------------- | ---- | ----- | ------------ |
| current fw (`g_cap`->1.0)        |  3%  |  0%   | 1725 (kept)  |
| always-sustain B (`STEADY_GCAP`=0.08) | 0% | 0% | 1261 (-27%)  |
| **goal (envelope gate)**        | 0%   | 0%    | ~1725 (kept) |

- **Option B** (`AEC_SUP_STEADY_GCAP` < 1, sustain the ceiling): drives echo to
  0% but is prediction-blind, so it crushes near-end (host `test_aec_fd`
  double-talk check fails, -12.8 dB) - barge-in dies.
- **Option A** (`AEC_SUP_GCAP_DTD_GATE`, release the sustain when
  `p_err >= ratio*p_ref`): a **pure-power** gate. It fires on ANY residual
  energy - so in a noisy room it releases constantly and the echo bleeds back.
  Host suite only passes it at a very sensitive ratio (0.1-0.2), which is
  exactly the noise-fragile regime. Dead end; left default-off.

### Envelope-gate plan (next)

Gate the sustained ceiling on **echo-vs-not by structure/correlation, not
power**, so noise cannot release it:

- Prefer keeping the cap ON (scramble). Release only on strong evidence of
  near-end VOICE. Priority order: (1) never let noise cause constant
  self-interruption -> bias toward staying capped; (2) barge-in may be a bit
  harder in noise (acceptable); (3) quiet-room barge-in must still work.
- Candidate signals (to prototype offline, none proven): short-window
  correlation of the residual envelope against the predicted-echo (`y`) /
  reference envelope; a coherence-keyed release; or a cheap near-end
  voice-structure detector on the residual with the predicted-echo removed.
  Each must PASS `case_noise` (stays ~0%) AND `case_nearend` (near-end kept)
  AND `case_echo` (0%), AND the host `test_aec_fd` double-talk + no-injection +
  HF-ERLE checks.
- Success = the goal row above with the host suite at 0 failures. Validate on
  dev kit bench (loopback) before any worn run.

### Mic-path insertion point

Both mic consumers drain 20 ms blocks (320 samples/ch @16 kHz) on a thread —
fine for block NLMS:

- **Lua path** (what the app's 16 kHz LC3 mic uses): `lua_microphone.c`
  `mic_thread_fn`, between `audio_microphone_read_owned()` and the LC3
  encode / ring-buffer store.
- **LE-Audio path** (the beamformer's future home): `ble_audio.c`
  `source_encoder_thread_func`, immediately after `audio_microphone_read()`
  and before the de-interleave/encode loop — the same spot reserved for
  `beamform_process()`, with AEC first.

The `aec_process()` implementation should live in `modules/halo/src/`
(mirroring `audio_eq.c` conventions) so both paths share it.

### Far-end reference tap

Tap the reference **in the MAX98357A driver at the I2S TX callback, after
`spk_protect_process()`** (`max98357a_audio.c`), into a small reference FIFO:

- It is the actual post-volume, post-protection block being clocked to the
  DAC, so the reference includes the limiter's (nonlinear, level-dependent)
  processing instead of asking the adaptive filter to model it.
- It collapses the bulk delay: tapping upstream at `audio_speaker_write()`
  sits before the I2S DMA queue (8 × 20 ms blocks ≈ up to 160 ms, occupancy-
  dependent), while the DMA completion callback is emission-exact. The
  measured emission-to-capture delay is only **~1–2 ms** (see the 2026-07-13
  root-cause chapter — the original 40–60 ms estimate was wrong, and below
  the 20 ms block granularity, hence the one-block mic hold-back while the
  speaker is active).

### Known constraints

- **Clocks are shared** (early drafts assumed independent oscillators —
  disproven by the 2026-07-12 clock audit above): the T5838 PDM mics and the
  MAX98357A I2S speaker both run from the crystal-derived `76M8_CLK`, measured
  within ~20 ppm of each other. No resampler and no drift tracking is needed;
  apparent drift in early captures was BLE feed starvation stretching the
  emission timeline.
- **CMSIS-DSP is wired in** as of the FDAF stage (selected by
  `CONFIG_HALO_AUDIO_AEC_FDAF`); MVE/Helium is confirmed enabled in the
  toolchain (`-mcpu=cortex-m55` → `__ARM_FEATURE_MVE=3`), so the FFT
  kernels vectorize. The STFT residual suppressor can reuse the same
  transforms.
- **Compute:** 640 taps × 16 kHz = ~10 MMAC/s scalar — comfortable on the
  M55 even without Helium.

### Bring-up order

1. Identify the echo path on real hardware (app sweep on the Halo unit; `/lfs`
   loopback on the dev kit) → confirm FIR length and distortion levels.
2. Tune the subtraction offline in this harness until ERLE / listening tests
   plateau.
3. Port stage 2 as `aec_process()` (int16 block NLMS + delay anchor), tap the
   reference at the I2S TX callback, hook the Lua mic path first (the app
   then directly A/B-tests the cleaned mic feed).
4. Flash and iterate on **the dev kit** first; flash the Halo unit only with explicit
   per-flash approval (see `.claude/skills/flash/`).
5. Residual suppression / beamformer chaining after the linear stage lands.

## Envelope release gate for the sustained ceiling (2026-07-15)

Executed the envelope-gate plan above. The sustained blanket ceiling now
persists past onset (`AEC_SUP_STEADY_GCAP` < 1) but is **released only for
genuine near-end voice** by a structure signal that stationary room noise
cannot trip - closing the mid-reply re-trip without killing barge-in. Host
suite **20/0** with the gate on; validated offline on the three real cases and
on the actual firmware. Flags default off; the device build enables them via
`modules/halo/CMakeLists.txt`. Tip: feat/aec (commit pending).

### The gate: echo-removed excess residual + a slow-floor onset detector

The key obstacle is that near-end voice and room noise are **both** "residual
not explained by the reference" - so any reference/power comparison (option A's
`p_err >= ratio*p_ref`) fires on both. The only separator is the *intrinsic
structure* of near-end voice (intermittent / syllabic) vs stationary noise. And
because the linear filter under-predicts the voice echo (the suppressor's `Sy`
is tiny next to the residual - the prediction `Y` is a weak signal), the echo
can't be removed from the residual using `Y`. It **can** be removed with the
strong reference envelope:

```
excess = max(0, sqrt(p_err) - KAPPA*sqrt(p_ref))     # KAPPA ~ echo coupling
```

`excess` is ~0 during echo-only (reference removes it), = the near-end during a
barge-in, and = the (stationary) noise floor in a noisy room. A **slow-floor
onset detector** then fires on voice but not noise:

- `fast` = fast EMA of `excess` (~2 blocks); `floor` = slow EMA (~2 s), which
  **tracks the ambient level with NO freeze**. Release when
  `fast > RATIO*floor` and `fast-floor > ABSFLOOR`, held by a ~1 s hangover.
- Stationary noise (even loud): `fast ~ floor` -> ratio test fails -> capped.
- Near-end burst over the low echo-only floor: `fast >> floor` -> released.
- Echo-only: `excess ~ 0` -> capped.
- **Freezing the floor during release is wrong** - it locks on to loud noise
  (a single transient freezes the floor low and everything after reads as
  release). The slow *tracking* floor is what makes loud stationary noise safe.

The sustain is additionally **scoped to `p_ref > PREF_MIN`**: idle/dither has
no reference echo, so the cap lifts and cannot inject (the option-B dither
failure). Params (all `#ifndef`, default off):
`AEC_SUP_GCAP_ENV_GATE`, `AEC_SUP_GATE_KAPPA` (0.15), `AEC_SUP_GATE_FAST_A`
(0.5), `AEC_SUP_GATE_FLOOR_A` (0.01), `AEC_SUP_GATE_RATIO` (2.0),
`AEC_SUP_GATE_ABSFLOOR` (0.5), `AEC_SUP_GATE_HANG` (50), `AEC_SUP_GATE_PREF_MIN`
(0.05).

### How it was tuned (real firmware, not the numpy mirror)

The numpy FDAF mirror (`offline_perbin.py`) does **not** align these worn
duplex captures (its xcorr drift-check fails; `ye ~ 0`), so it is useless for
gate tuning here. Instead the discriminator was tuned on **per-block
`p_err`/`p_ref` dumped from the actual firmware**: `host/aec_wav.c
-DAEC_WAV_DUMP` prints `off sy se perr pref gmean` per block, and a scratchpad
sweep evaluates gate variants on the three real cases. Result: **87 gate
configs** cleanly separate the near-end window (fires >90%) from loud noise
(2%) and echo (0%). `re/sqrt(p_ref)` alone (= option A) does separate noise
(4.0) from echo (0.14) but near-end sits on the echo floor (0.15) at the median
- confirming structure, not power, is what works.

### Results table

Offline (`host/aec_wav` + `silero_score.py`, `-DAEC_WAV_DUMP` off), and the host
`test_aec` oracles:

| variant                              | echo | noise | near-end RMS | host suite |
| ------------------------------------ | ---- | ----- | ------------ | ---------- |
| current fw (`g_cap`->1.0 after onset) |  3%  |  0%   | 1179 (kept)  | 20/0       |
| option B (`STEADY_GCAP=0.08`, no gate)| 0%   | 0%    | **304 (crushed)** | 3 FAIL |
| **env gate + `STEADY_GCAP=0.25`**     | 0%   | 0%    | **1179 (kept)** | **20/0** |

- Near-end RMS is the voice-band `[76.5,82.4]s` barge in `case_nearend`. The gate
  releases there, so its output equals the no-cap baseline (1179); option B
  crushes it to 304 (-11 dB). Host double-talk: -1.6 dB (gate) vs -12.8 dB (B).
- The gate **holds** on `case_noise`: output voice-band RMS 1511 ~ option-B's
  1474 (both capped), well below the 1690 uncapped baseline - continuous noise
  does not release it.
- Idle-injection and HF-noise ERLE (the noise/stability oracles): 0.8 dB and
  5.3 dB, both pass.

### Depth floor: STEADY_GCAP >= 0.25 (the HF-ERLE kernel-fold wall)

`STEADY_GCAP=0.25` is the **deepest** cap that clears the host HF-noise ERLE
oracle (5.3 dB @0.25, 4.8 @0.20, FAIL below). A deeper in-band cap fails it
because the suppressor's 257-tap gain kernel realizes the hard in-band/out-band
step imperfectly and **folds the (near-Nyquist) HF mic noise back into the
voice band** - and the scoring, which subtracts the known additive noise at
unity, reads any such modification as lost cancellation. Extending the cap to
the above-band bins to remove the step (`CAP_HF`, tested) made it **worse**
(-5.0 dB): capping HF crushes the expected in-band noise the scorer subtracts.
So the sustained scramble is -12 dB, not the -22 dB of option B's 0.08. Whether
-12 dB is deep enough to keep the **63 s misaligned barge** (a larger,
structured residual - see the FDAF-telemetry chapter) below Silero is a worn-
test question; offline stays reference-aligned so it can't reproduce it, and at
-12 dB the aligned `case_echo` is already 0%.

### Telemetry + reference-EQ caveat

- Per-block gate telemetry through `audio_aec_snapshot()` -> `aec('stats')`:
  `sup_gate_fast`, `sup_gate_floor`, `sup_gate_rel` (1 = released this block).
  For the worn test: `sup_gate_rel` should read 0 while the wearer is silent
  (echo scrambled) and 1 when they barge in.
- **The offline reference is pre-EQ** (a caveat surfaced this session, not a gate
  issue): the on-device AEC reference tap is correctly **post-EQ** (the
  MAX98357A driver's voicing EQ / HPF / limiter, `spk_protect_process()`, then
  `tx_tap_sent` on the same buffer). But `duplex_ref_*.wav` is a plain host
  `lc3.Decoder` output with none of that, so every offline reference-vs-mic
  analysis (coherence ~0.40, the "prediction is a dead end" cross-set CV,
  `aec_wav`) compares the echo against a reference that differs from what was
  emitted. It depresses measured coherence and partly confounds the
  prediction-dead-end conclusion (the device-side ~5 dB voice ceiling, measured
  with the correct tap, still shows a real low-coherence limit). The gate is
  downstream of the linear filter and unaffected; but a future offline run
  should apply the driver EQ to the host-decoded ref (or stream the device's
  actual tap out).

### Next

1. **Worn vol-80 duplex re-test** (needs a worn session): confirm the gate releases on
   his barge-in (`sup_gate_rel`->1) and stays capped (->0, Silero ~0%) while he
   is silent through a full reply, especially past 63 s. Tune `KAPPA` if the
   on-device echo coupling differs from the 011535 capture (robust 0.09-0.3
   offline).
2. If -12 dB proves too shallow for the misaligned barge, attack the **63 s
   reference-margin collapse** directly (the FDAF-telemetry chapter's item 1) -
   a deeper cap can't clear the HF-ERLE oracle, so the misalignment is the
   better lever.
3. Then: do-no-harm leak, beamformer chaining.

## Worn re-test: the blocker is mic-DELIVERY latency, not the AEC (2026-07-15)

Ran the worn vol-80 duplex re-test (Halo unit, gate build, `--auto-prompt`
`--auto-interval 10`). **The gate did not stop the self-interruptions worn**, and
the diagnosis moved the problem entirely OFF the AEC. Device has been reverted to
known-good (gate off); all gate code stays flag-gated default-off.

### What the telemetry showed

- **0-~40s clean** (margin +43, `sup_gate_rel=0` while silent - the gate holds
  capped correctly), then the reference `margin` collapses (< 0 and stays),
  `ref_underruns` climb ~50/s, `p_ref` reads ~0 during replies. Cancellation
  dies -> the full structured echo re-trips Silero every 1-2s (the wearer's report).
- The gate's `sup_gmean=1.0` wherever `p_ref~0` exposed an interaction bug in the
  PREF_MIN sustain-scoping: a *collapsed* reference reads p_ref~0 and was
  mistaken for *idle*, LIFTING the cap exactly when the uncancelled echo needed
  scrambling. Fixed with a fail-safe duck (`AEC_SUP_REF_UNREL_HOPS`: hold + deepen
  the cap while margin < 0) - which cut the false barges (a 54s clean stretch) but
  **kills real barge-in** (with no reference during the collapse, echo and
  near-end are indistinguishable; the wearer's barge failed at 150s). The duck is a
  documented dead-end, left default-off.

### The real cause: cap_late_ms grows to 15-20s (the mic timeline falls behind)

`cap_late_ms` (capture-epoch lateness = `now - mic_total/16` at
`audio_aec_process`, BEFORE LC3/BLE) climbs **monotonically 0 -> ~16,000-20,000
ms** at ~34-74 ms/s and never drains. That 15-20s mic backlog IS what the wearer felt
("barge-in took ~15s to kick in" - the server heard him 15s late). The growing
mic timeline is what drifts the reference window off the ring (the margin
collapse is a *symptom* of it).

**The AEC is exonerated.** `--aec-off` (`frame.microphone.aec(false)` - the
accounting runs regardless, the filter returns early so zero AEC compute) STILL
grows `cap_late_ms` (17,067 -> 20,336, ~66 ms/s). PDM side is clean: `pdm_popped`
at exactly 16 kHz, `pdm_discarded=0`, `mic_lost=0`. So the backlog is between PDM
capture and the mic thread (`lua_microphone.c` `mic_thread_fn`) calling
audio_aec_process - the thread is not keeping pace under full-duplex BLE load and
once behind never catches up (flat-then-growing, never drains). Aggravated by the
rapid auto-prompt (back-to-back replies saturate the speaker downlink -> the mic
uplink/thread starves). Prior CLEAN runs had `cap_late_ms` FLAT (on-desk 033020 =
77 const 300s; head-worn PRE-gate 004647 = 0 const - though 004647 still had the
margin collapse WITHOUT cap_late growth, so there may be two mechanisms).

### Next session: mic-delivery latency (its own focus, separate from the AEC)

1. Locate the backlog: `audio_microphone_read_owned` pacing/FIFO semantics; the
   mic thread priority (`CONFIG_HALO_LUA_MICROPHONE_TASK_PRIORITY`) vs the BLE
   controller under duplex; the LPPDM DMA-drain interaction.
2. Bound the latency: when the mic thread falls behind it must DRAIN (skip to
   newest) rather than accumulate - cap capture->process lag at <100ms so the
   reference stays aligned and the (validated) gate can actually work.
3. Confirm whether realistic reply spacing (`--auto-interval 25+`) avoids it - is
   this a stress artifact or a normal-use bug?
4. Then re-enable the gate (`AEC_SUP_GCAP_ENV_GATE=1`, `STEADY_GCAP=0.25` in
   `modules/halo/CMakeLists.txt`) and re-test worn.

## The "mic-delivery latency" was a metric artifact; the real blocker is a clock-skew margin collapse (2026-07-14)

The previous chapter's diagnosis was **wrong on its central claim**, and this
chapter corrects it and lands the actual fix (default-off, host-validated).
`cap_late_ms` is **not** a physical mic backlog - it is a diagnostic-only
accumulator - and the mic is delivered in real time. The worn barge-in blocker
is the reference-**margin** collapse driven by the independent I2S-vs-PDM
hardware clocks.

### `cap_late_ms` is a bookkeeping accumulator, not a latency

`cap_late_ms` is written in exactly one place, `epoch_observe()`, and increments
only when `obs = now - mic_total/16` dips *below* the epoch frozen in the first
2 s - i.e. when the mic timeline runs slightly *ahead*. It is a running
**per-block sum** of that undershoot, and it feeds **no** control logic (grep:
struct / reset / epoch_observe / stats only). A real backlog (mic *behind*)
makes `obs` go *above* the epoch and increments nothing.

Re-derived from the saved `duplex_events_*.jsonl` (the actual worn runs):

| run (gate build) | `inst_micHz` | `obs = now-mic_total/16` | `cap_late_ms` |
| ---------------- | ------------ | ------------------------ | ------------- |
| 114404 (245 s)   | **16000.00** | **flat ~4775 ms** (drift 13 ms) | 125 -> 16781 |
| 004647 (clean)   | 16000.00     | flat ~4320 ms            | **0**         |

`obs` drifted 13 ms while `cap_late_ms` "climbed" 16 781 ms - a 1290x
disagreement. The only difference between the runs is where the startup epoch
froze (~1 ms above vs at the steady `obs`); that constant ~1 ms undershoot
re-summed at 50 blocks/s *is* the entire "climb". `mic_lost=0`,
`pdm_discarded=0`. So `--aec-off` still growing `cap_late_ms` just confirms it
is the (harmless) accounting, not that the mic path is the culprit. The PDM
DMA-drain path is healthy and real-time; there is no backlog to drain.

### The real blocker: reference margin walks negative under clock skew

Both runs (incl. the clean one) show `ref_underruns` climbing ~50/s and
`margin` -> -133. `ref_consume()` anchors the reference **read** window on the
mic capture clock (16000/s, never stops) while the write head `w` advances only
on emission (~15996/s in the logs). The two are independent Balletto clock
branches ~50-250 ppm apart, so `margin = w - desired_end` drifts negative and
the newest ~40 % of each block's reference is silence-padded -> partial
cancellation -> structured echo leaks -> Silero self-barge. Emit-side telemetry
rules out BLE feed starvation: `feed_gaps`=1 (lead-in only), `emit_late=0`, only
slow `emit_slips` (+46 ms/245 s = the residual skew the existing backstop
under-corrects). This is the same phenomenon the FDAF chapter called "the 63 s
barge is reference misalignment".

### The fix: a consumer-side margin servo (`AEC_MARGIN_SERVO`, default off)

Holding `margin` at a constant target keeps the echo at a **fixed tap**
regardless of drift (`echo_tap = physical_delay*16 - margin`, independent of
both clocks), so the echo never walks out of the causal window. The servo, in
`ref_consume()`:

- targets the margin captured at the last re-anchor (`margin_anchor` - the
  epoch-chosen geometry: +75..+187 on device, ~0 on the host harness), so it
  disturbs nothing on a skew-free stream;
- when margin drifts a deadband (24) below target, reads the reference a touch
  slower - advances the read pointer by <= 2 samples/block less - so the window
  re-reads a sample or two and margin climbs back; catches up symmetrically when
  it runs high;
- moves the read pointer **and** `desired_end` by the same accumulated amount
  (`ref_skew_adj`), so `drift` is **invariant** and the servo can never trip the
  >160 resync; re-anchors to the raw epoch position so a re-engage is identical
  to the servo-off path.

It lives entirely on the mic thread (no cross-thread write to `emit_epoch`).
New telemetry: `ref_skew_adj` in `aec('stats')` (accumulated skew correction).

### Validation

- **Host suites green both ways.** `test_aec` + `test_aec_fd`, with and without
  `-DAEC_MARGIN_SERVO`: 20/0. Servo-off is `#if`-guarded to a byte-identical
  build.
- **Skew simulator** (`host/skew_sim.c`, drives the real `audio_aec.c` with a
  controllable emit-vs-mic ppm skew the lockstep suite can't create). Over
  240 s, `ref_underruns / ref_pads` at the end:

  | ref clock skew | servo OFF        | servo ON   |
  | -------------- | ---------------- | ---------- |
  | 60 ppm         | 1075 / 11646     | **0 / 0**  |
  | 250 ppm        | 1980 / 40240     | **0 / 0**  |
  | 500 ppm        | 3582 / 116064    | **0 / 0**  |

  Off: `margin` sawtooths +172 -> negative -> resync, padding storm throughout.
  On: `margin` holds ~104-168, `ref_underruns`/`ref_pads` stay **0**;
  `ref_skew_adj` ramps to track the skew. (Resyncs are unchanged - they come
  from the emit-epoch backstop stepping the epoch, not the servo - but at 1-9
  over 240 s they are gentle vs the continuous underrun storm they replace.)

### Next

1. Enable `AEC_MARGIN_SERVO` in `modules/halo/CMakeLists.txt`, dev kit-bench, then
   worn vol-80 duplex: confirm `margin_last` holds positive and `ref_underruns`
   stay flat past 63 s (was the collapse point).
2. With margin held, re-enable the envelope gate (`AEC_SUP_GCAP_ENV_GATE=1`,
   `AEC_SUP_STEADY_GCAP=0.25`) and confirm `sup_gate_rel`->1 on barge-in, ->0
   (Silero ~0%) while silent through a full reply.

## Worn validation: the servo works; the gate breaks the loop but deforms barge-in (2026-07-14)

Worn vol-80 duplex on the Halo unit (worn), servo and gate tested as a clean
A/B (servo alone first, then servo+gate). New live telemetry in
`duplex_vad_test.py` (`ref_skew_adj`, `sup_gate_rel`, `ref_underruns` in the
`[stats]` brief). Both mechanisms are confirmed working on real hardware; one
tuning problem remains, cleanly characterised.

### Servo alone (gate off) — the alignment collapse is fixed

| metric | baseline (collapse) | servo (170s worn) |
| ------ | ------------------- | ----------------- |
| `margin_last` | walks to -85..-133, pinned | **+47..+191, always positive** |
| `resyncs` | 3-4 | **0** |
| `p_ref` during replies | ~0 (collapsed) | **real (197-510)** |
| `ref_underruns` / `ref_pads` | ~50/s, ~5000/s | ~1.2/s, ~36/s (~40x fewer) |
| `ref_skew_adj` | - | ramps 2->296 (servo tracking the real skew) |

`ref_skew_adj` ramping proves the Halo unit has a large, bursty I2S-vs-PDM skew the dev kit
bench could not show (dev kit: margin constant, `ref_skew_adj=0`). **the wearer's barge-in
was detected immediately - no ~15s lag** - retroactively confirming the "15s"
was the cancellation dying from the margin collapse, never a mic backlog. The
servo is an unqualified win and is now **enabled in the device build**.

### Servo + gate — the self-interruption loop is broken, but the release is too slow

Gate confirmed active: `sup_gmean` driven to **0.03-0.20** during the
deep-echo replies (capped/onset-ducked), lifting to 1.0 between replies
(`p_ref`~0), and **released** (`sup_gate_rel=1`) on the wearer's genuine barge-ins.
Over 215s / 16 replies: **false barges 16 -> ~6, and no runaway loop** (the wearer
confirmed by ear: assistant echo "crushed almost all the time", no recurring
self-interruption).

**The remaining problem (the wearer's listening test of `duplex_mic_140338.wav`):**
- Assistant echo crushed almost always (good); his own speech undeformed while
  the assistant is silent (the `PREF_MIN` lift working).
- **But the first ~2s of every barge-in is severely deformed.** The gate's
  near-end release needs ~2s of structure to distinguish his voice from echo
  before it lifts; until then his voice is scrambled too. So a short "stop"
  never survives to Silero, and longer barge-ins only clear ~3s in.
- This is the gate, not the harness: at t=94.5 the gate released
  (`sup_gate_rel=1`) and s2s Silero fired `speech_started` at **94.7 - 0.2s
  later**. Silero is prompt *once the gate lets the voice through*; the ~2s is
  the gate's release latency. (A secondary contributor: speaker-downlink audio
  already queued over BLE plays out for a few hundred ms after the server
  decides to stop.)

So the same mechanism that scrambles the echo (stops self-interruption) also
scrambles the barge-in onset (defeats short barge-ins) - opposite goals. The
gate stays **default-off** until this is fixed.

### Next: tune the gate-release latency (its own session)

Goal: fire `sup_gate_rel` on a genuine near-end onset in **~300-500ms** (not
~2s) so the barge-in onset survives to Silero, WITHOUT stationary echo/noise
tripping a false release (which would bring the self-interruption loop back).
Knobs (all in `audio_aec.c` `fd_process_block`, `#ifndef`): `AEC_SUP_GATE_HANG`
(hangover to fire), `AEC_SUP_GATE_FAST_A` (fast-EMA responsiveness),
`AEC_SUP_GATE_RATIO` / `AEC_SUP_GATE_ABSFLOOR` (release thresholds). Oracle is
worn (host can't skew the clocks or reproduce the double-talk onset); watch
`sup_gate_rel` latency on barge-in vs false barges while silent, and re-listen
to `duplex_mic_*.wav`. Then re-enable the gate in `modules/halo/CMakeLists.txt`
alongside the servo.

## Gate-release latency fix: a rising-edge path (2026-07-14)

The ~2s release latency traced to the *level* test alone:
`fast > RATIO*floor && (fast-floor) > ABSFLOOR`. The floor is a slow (~2s) EMA
with no freeze, so during a reply it tracks up to the steady echo-excess level
`F`. A barge-in then only fires once `fast > 2*floor` - i.e. the near-end voice
must **out-power the echo residual**. On-device `F` is large (the reference
under-cancels, worse under the clock skew the servo only partly tames), so a
soft "stop" never clears the bar and a loud barge-in takes ~2s of sustained
voice. This is a level-latency floor that no amount of `HANG`/`FAST_A` tuning
removes (the fire is immediate once `active`; the latency is entirely in
`active` going true late).

**Fix: an orthogonal rising-edge path, OR-ed with the level test.** A medium EMA
`gate_mid` (`AEC_SUP_GATE_MID_A` ~= 0.08, a ~240ms baseline) tracks the recent
excess; the edge fires when

    (fast - gate_mid) > AEC_SUP_GATE_EDGE_ABS  &&  fast > AEC_SUP_GATE_EDGE_RATIO*gate_mid

A genuine voice **onset** spikes `fast` above its 240ms baseline within 1-3 hops
(~20-60ms), before `gate_mid` catches up; steady echo and stationary noise sit
at `fast ~= gate_mid` (no edge). Because the excess already subtracts
`KAPPA*sqrt(p_ref)`, an echo syllable's own envelope is largely cancelled, so the
edge is near-end-selective. The 240ms baseline (vs the level test's 2s floor)
also dips in the gaps between the assistant's syllables, so a barge-in lands
sooner even where the level test is still floor-bound.

**Offline tuning (`host/aec_wav -DAEC_WAV_DUMP` now dumps `gate_fast gate_mid
gate_floor gate_rel`; three cases sharing `duplex_ref_011535.wav`):** the
discriminator is `EDGE_RATIO`, not `EDGE_ABS` (the excess magnitudes are large,
so the absolute guard is inert vs real events and only blocks the near-silence
`mid~=0` case). `case_noise` false-release: RATIO **1.3 -> 11.7%** (5 multi-block
raw trips), RATIO **1.8 -> 0 raw trips** (== the validated level-only 2.4%
baseline), while `case_nearend` still fires 9x across the burst (coverage 95%,
up from the level-only 90.7%). `case_echo` stays 0%. So the shipped default is
**`EDGE_RATIO=1.8`, `EDGE_ABS=0.6`, `MID_A=0.08`**. Both host suites stay green
(the gate is `#if`-compiled-out of the 16-check TD/FDAF builds).

**Offline cannot validate the latency win** - offline echo cancels well enough
that even the *level* path fires at 60ms on the near-end onset; the ~2s only
appears on-device from the reference under-cancellation. So this is a worn oracle:
watch `sup_gate_rel`/`sup_gate_fast`/`sup_gate_mid` (all in the `[stats]` brief)
- release should fire within ~300-500ms of a real barge-in and stay 0 (low false
barges) while the wearer is silent through a reply. If a real barge-in is still slow,
lower `EDGE_RATIO` toward 1.3 and re-check the false-barge count; if noise starts
self-interrupting, raise it back. New telemetry field: `sup_gate_mid`.

### Worn result (Halo unit vol-80, 2026-07-14): release fixed; a new reference-collapse follow-up

Worn-validated with the edge path enabled: a short loud **"stop" now stops the
assistant AT ONCE** (the ~2s scramble is gone), and once the reference is warmed
up, full replies play through with **no self-interruption** (loop stays broken).
Live `sup_gate_rel=1` fires on the wearer's voice (`sup_gate_fast` jumps to ~1.9
from the ~0.04 idle floor). This is the wearer's ear + the live gate telemetry;
**offline `aec_wav` reprocessing is NOT a valid latency oracle** here (it shows
~0.7% release because the perfectly-aligned reference cancels the echo too well
to reproduce the worn excess - the long-standing "offline is blind to the gate"
limitation).

**New problem surfaced (separate from the gate release, logged as follow-up):**
a barge-in can trigger a **transient reference feed-starvation loop**. During it
`p_ref` collapses to ~0 while `margin` stays **positive** (~35) - the speaker
feed stalls under the barge-flush churn so the newest reference is silence, which
reads as "idle" and lifts the sustained cap (`gmean`->1.0), so the *new* reply's
echo self-interrupts for ~20s before the reference re-anchors and it recovers.
The `ref_unrel_hops` fail-safe misses it because that keys on `margin < 0`, not
on feed starvation with positive margin. Reprocessing the captured (mic, ref)
offline shows the reference DATA is healthy (`p_ref` med ~80-110, cap engages) -
the collapse is purely a *live* feed/alignment event. Worst before warm-up and
under rapid barging (an aggressive `--auto-interval 8` provoked it hard;
`--auto-interval 18` with deliberate spaced barges is stable). Candidate fixes
for a later session: detect a write-head/feed stall (not just `margin < 0`) and
hold the cap through it; or don't lift on `p_ref < PREF_MIN` while playback is
known-active. The `AEC_MARGIN_SERVO_STEP 2->4` polish may also help the reference
recover faster.

#### Fix: playback-active hold on the p_ref collapse (worn-validated 2026-07-14)

The chosen fix is the second candidate made robust - "don't lift on
`p_ref < PREF_MIN` while playback is known-active" - with a playback-active
signal that is independent of both `p_ref` AND the feed timestamp. The naive
"was the tap fed recently?" test fails here: the round-8 speaker driver silence-
feeds continuously between replies to keep the reference timeline engaged, so
`feed_stamp_ms` stays fresh even when idle. The distinguishing signal is the
POWER of what is being WRITTEN to the ring: during the collapse the new reply's
real audio is written at the write head (the saved runs show `spk_real_sends`
climbing ~1000/10s) while the aligned read window serves silence
(`p_ref`->~0) - a write-vs-read power divergence.

`audio_aec_feed_reference` now stamps `real_feed_stamp_ms` only when the emitted
block's power clears `AEC_REF_GATE_RMS` (the same gate adaptation uses; silence-
feed never trips it, integer-only so no float in the tap ISR). The steady-cap
block splits the old `p_ref < PREF_MIN || gate_released` lift into three: keep
the fail-safe (`ref_unrel_hops`), always release on genuine near-end
(`gate_released`, unchanged - a barge-in still lifts), but on a bare
`p_ref < PREF_MIN` only lift when the last REAL feed is older than
`AEC_SUP_PLAYBACK_HOLD_MS` (400ms). Recent real playback + collapsed `p_ref` =
the barge-flush event -> HOLD the echo-scramble cap. New telemetry `sup_pb_hold`
= 1 on the blocks the guard fires (doubles as the flash-landed probe:
`probe_pb_hold.py`). NOTE `sup_gmean` is the mean of the PRE-cap Wiener gains, so
during a collapse (predicted echo `Sy`~0, Wiener gains ~1) it reads ~1.0 even
though the applied `g_cap` ceiling is holding the output kernel at
`STEADY_GCAP` - `sup_gmean` is NOT the lens for whether the cap is scrambling
here; `sup_pb_hold` + the ear are.

Host suites stay green in all three builds (TD/FDAF/gate 20/0) - the guard is
inert offline where the reference is perfectly aligned (`p_ref` healthy, the
branch isn't taken) and `sup_pb_hold` is byte-for-byte off. Built clean, flashed
+ verified + probe-confirmed on the dev kit and the Halo unit.

#### The second collapse mode: margin-negative, and the servo step 2->4

Worn Halo unit vol-80 surfaced that the p_ref collapse has TWO regimes, split by
the sign of `margin`:
- **margin POSITIVE** (write head advancing, aligned window just serves silence
  briefly): the pb_hold guard above holds a SHALLOW cap (`STEADY_GCAP` -12dB)
  and, crucially, does NOT re-arm the onset duck - so `gate_released` still lifts
  cleanly and near-end voice is preserved. This is the good, near-end-friendly
  path.
- **margin NEGATIVE** (the read window slid OFF the write head): the old
  `ref_unrel_hops` fail-safe fires and re-arms the deep onset duck (`ONSET_GCAP`
  -34dB), which scrambles NEAR-END too - the known ref_unrel dead-end. Worn this
  showed up as "stop stops working": under aggressive barge-flush churn the
  step-2 margin servo was overrun (run `duplex_*_160319`: `margin_last` -701,
  `margin_min` -885, `ref_underruns` 20034, `ref_skew_adj` stuck at 24 - clawing
  back only ~2 samples/block), so margin sat negative and the deep duck held the
  wearer's voice down.

Fix: **`AEC_MARGIN_SERVO_STEP` 2->4** (modules/halo/CMakeLists.txt; was the long-
deferred polish, now data-forced). Doubling the claw-back keeps margin positive
under flush stalls so collapses stay in the near-end-friendly pb_hold path rather
than falling into the near-end-killing ref_unrel path. Still <=4/320 samples
re-read per block (inaudible). Host unaffected (host builds set their own defines,
not this CMakeLists; `skew_sim` has more headroom at a larger step).

Worn re-test (Halo unit vol-80, manual driving + rapid-barge stress, run
`duplex_*_161843`, 332s): **`margin_min` -61** (was -885), **`resyncs` 0** (was
4), **`ref_underruns` 111** (was 20034), **`ref_pads` 2055** (was 6.04M),
`ref_skew_adj` ramped to 616 (the servo actively defending margin), and the
ref_unrel deep duck (`sup_onset`>0) fired in only a handful of brief samples
instead of sitting pinned at 49. the wearer's ear: "stop stayed effective through the
rapid barges usually" - the assistant's own voice doesn't always cut off
instantly (that residual is the SEPARATE gate-release latency, not this
collapse), but the wearer's barge reaches the server. Both levers committed.

## The residual slow-stop was a client-side flush gap, NOT the gate (2026-07-14)

The remaining worn complaint - "my 'stop' works, my transcript reaches the
assistant, but the assistant's own voice doesn't always cut off right away" - was
scoped as gate-RELEASE latency (lower `AEC_SUP_GATE_EDGE_RATIO`). Diagnosing the
two saved captures first (`duplex_*_161843` clean, `duplex_*_160319` aggressive)
showed the gate is NOT the bottleneck:

- **The gate releases correctly.** On every barge cluster `sup_gate_rel`->1 and
  `sup_gate_fast` spikes to 2-6 (vs ~0.04 idle). All 33 barges across the two
  runs produced a `speech_started` - the wearer's voice cleared the AEC and
  reached the server VAD every time. "Transcript reaches the assistant" == the
  gate is doing its job; detection was never the problem.
- **Nothing stopped the buffered playback.** The realtime server generates the
  WHOLE reply ~1.4s (median; max 2.0s) after `playback_start` and closes it with
  `response.done: completed`. The reply then plays for ~6.4s median (up to 17.8s)
  as the client dribbles its `speaker_queue` over BLE at ~real-time pace (up to
  ~8s of audio queued client-side). **0 of 33 barges** landed inside that ~1.4s
  generation window - every one arrived AFTER `response.done: completed`, so the
  server-side auto-interrupt had nothing left to cancel. With `onset_guard_ms=0`
  (the worn config) the client-side `_client_barge_in` flush - the only thing
  that drops the queued backlog - was disabled. So a barge after the first ~1.4s
  of any reply couldn't be stopped by anything; the reply drained in full and the
  barge's OWN new reply stacked on the queue. That is the felt "voice keeps
  going". Lowering `EDGE_RATIO` can't fix it (detection already fires; a faster
  gate only helps a barge inside the ~1.4s window, and none are).

### Fix: always client-flush a confirmed barge, with a mic-RMS debounce

`duplex_vad_test.py` (harness only - no firmware change, `audio_aec.c` /
CMakeLists untouched, host suites unaffected):

- **Client flush in default mode.** A `speech_started` during active playback now
  drains `speaker_queue` (client-side barge) even when the server has nothing to
  cancel. This is what actually stops the ~8s BLE backlog.
- **A couple-of-readings debounce (product ask).** A single VAD trip must not cut
  off the assistant. `speech_started` only ARMS the flush; `pump_mic` then
  requires `--barge-confirm-frames` (default 2) consecutive mic reads whose
  DC-removed (AC) RMS clears `--barge-rms` (default 150) before draining. The
  client mic RMS is the immediate near-end signal - `speech_stopped` is useless
  here because it lags by the server VAD's silence hangover (~500ms). NB the
  device mic carries a large persistent DC bias (~1265 across the whole capture,
  independent of the first-second PDM start-pop), so a RAW RMS is swamped by DC
  and must be measured as `np.std` (AC). Quiet mid-reply floor ~40-70 AC-RMS,
  near-end onset median ~647 - 150 separates them cleanly.
- **`barged` latch reset.** The pre-existing latch that drops a cancelled reply's
  trailing deltas was keyed ONLY on `response.created` to clear - but s2s in
  server-VAD mode never emits `response.created` for auto-created replies, so
  after the first barge the latch stuck and ALL later audio went silent. Fixed:
  track `response_active` off the audio stream (a delta means live, `response.done`
  ends it), clear `barged` on `response.done`, and only latch it in
  `_client_barge_in` when a reply is genuinely still generating (a post-completion
  barge - the common case - has no tail to drop and must not mute the next reply).

### Worn result (Halo unit vol-80, 2026-07-14, run `duplex_*_181709`, 312s)

the wearer driving manually, deliberate spaced barges. 16 replies, 9 barges over active
playback: **8/9 stopped promptly** (barge->flush ~50ms; 5 via the debounced client
flush dropping 152-499 frames, 3 via the server cancel), **11/16 replies played
fully with no barge inside** (no false cutoff while silent). Correlating each barge
against the recorded near-end mic RMS: **all 5 client-flushes were genuine barges
(RMS 658-1189) - zero false client-flushes.** the wearer's ear: a couple-of-words barge
reliably stops the voice before full playout; a *very short* "stop!" can be too
brief to clear the 2-read debounce (1 missed); ~one self-interruption in the whole
run. That single self-interrupt is `t=182.3, near-end RMS 108` (below the debounce
floor - echo residual, not the wearer): it cut the reply via the **server's
un-debounced auto-interrupt** (`interrupt_response=True` cancels on a single Silero
trip), which the client debounce can't gate. **Follow-up lever:** route ALL barges
through the debounced client flush by disabling the server auto-interrupt (the
`onset_guard`-style path) - the client flush already covers post-completion barges
and `response.cancel` still stops mid-generation ones, so the un-debounced server
interrupt may be pure downside. Knobs to trade the missed-short vs self-interrupt
rates: `--barge-confirm-frames`, `--barge-rms`. The margin servo held through the
heavy barge churn (`margin_min` -157 -> recovered +3, `resyncs` 0, `sup_pb_hold` 0
- collapse guard never needed).

## Voice mode: the self-interruption was out-of-band echo, not onset timing (2026-07-15)

The residual self-interruption (the follow-up left by the previous chapters -
"one self-interrupt per run", blamed on the server's un-debounced auto-interrupt)
turned out to have a deeper, different root cause. Chased end-to-end this session
and fixed on-device; `feat/aec` now holds a full-duplex conversation with
barge-in and essentially no self-interruption. Worn-validated the Halo unit vol-80.

### DC detour: remove it on the AEC OUTPUT, not the PDM front-end

The mic carries a ~1265-count persistent DC bias because the Halo PDM node
bypasses its HW DC-blocking IIR (`halo.dts`: `iir-bypass; fir-bypass;`). First
tried un-bypassing the HW IIR - it removed the DC but drove reference-alignment
churn (**`resyncs` 0 -> 21** over a session, `margin_min` decaying), because it
puts an asymmetric high-pass on the mic only, ahead of the canceller, that the
band-limited adaptation can't model. Reverted. The AEC is DC-blind by design
(adaptation band-limited to `[FD_BIN_LO,FD_BIN_HI]`, DTD/mic-power measures use
high-passed views), so DC just rides through the bit-faithful output. Fix
(committed): a one-pole DC blocker on the AEC OUTPUT (post-suppressor, pre-LC3,
`mic_dc_block` in `lua_microphone.c`) - `resyncs` back to 0, mic DC -0.00 through
the full LC3+BLE path, startup transient smaller than the HW IIR's.

### The probe: the onset duck was never the problem

Instrumented the suppressor with a temporary onset-leak probe (latched the emitted
block's AC-RMS + `ob`/`g_cap`/`gate_released`/`pb_hold`/`p_ref`/`p_err` whenever a
loud block landed in the onset window; surfaced via `aec('stats')`, removed after
the diagnosis). It was decisive and overturned the working theory: at EVERY leak,
**`ob=1.0` and `g_cap=0.02`** - the deep onset duck was fully armed - yet the block
emitted 150-500 RMS, with the in-band `p_err` tiny. The leak is therefore
**out-of-band**: the suppressor caps only `[312,3800]Hz`, and everything outside
passes at unity gain.

### Spectral proof (mic wav split by ref-playing)

Classifying loud mic frames by whether the assistant was playing (`duplex_ref`):
echo-leak frames were **~71% above 3.8kHz** vs 3.3% for near-end, and a
finer split showed a bone-conduction echo peak at **220-312Hz (~57%)** - below the
adaptation floor, so never cancelled, and speech-shaped enough to trip Silero.
Near-end voice peaks higher (360-800Hz). The leaking echo is the assistant's own
voice, so no amount of level reduction that leaves it speech-shaped will stop the
VAD - it has to be removed or ducked in-band.

### The fix stack (all committed on feat/aec)

- **Voice mic mode** (`45b95ae`, `mic_voice_bandpass`): a 4th-order Butterworth
  band-pass `[300,3400]Hz` on the AEC output, confining the mic to the band the
  AEC actually processes so out-of-band echo can't reach the VAD. Opt-in
  (`frame.microphone.start{voice=true}`; `+200` on the `START_LISTENING` code;
  `duplex_vad_test.py --no-voice` to disable) so a developer can still take the
  raw full-band AEC'd feed. 2nd-order was too gentle (echo hugged the band edges,
  ~230 RMS); 4th-order killed the >3.4kHz side but the 220-312Hz peak remained.
- **Low-band cap** (`8f5c351`): extend the suppressor's blanket ceiling DOWN to
  `FD_CAP_BIN_LO` (~203Hz), below the adaptation band, prediction-independent (no
  per-bin Wiener gain, just `g_cap`) so it can't destabilize the NLMS the way
  band-limiting the ADAPTATION this low would. Ducks the bone-conduction echo at
  the source; lifts with the in-band gate on near-end/idle. **Internal AEC-output
  echo dropped ~500 -> ~155 RMS; self-interrupt spirals collapsed to ~1 trip.**
- **Shortened onset duck** (`dd44ff4`, device-scoped in `modules/halo/CMakeLists.txt`):
  `AEC_SUP_ONSET_HOPS 175->45`, `AEC_SUP_ONSET_HOLD_HOPS 125->20` (~2.5s -> ~400ms).
  Because the band-pass confined near-end to the ducked band, the long onset hold
  (sized for the cold-start first reply) crushed genuine barge-ins for seconds.
  Warm restarts reconverge in a few hundred ms and the low-band cap + band-pass
  now carry the onset echo, so a short hold suffices. Server cancels stayed at 2
  (no self-interrupt regression) while barge-ins land with only the first ~400ms
  ducked.

### RMS flush: keep it

Re-ran with the client mic-RMS barge path disabled (`--barge-rms 99999`, relying
on the server cancel + backlog flush alone): **only 1 server cancel in 5 min and
barges felt weak / some missed**. The client RMS flush is pulling real weight for
barge responsiveness (immediate backlog drain vs the server VAD's hangover) - keep
it. Note the mic stream is identical either way, so this is about flush timing,
not audio.

### Follow-ups

- A barge held CONTINUOUSLY across several rapid reply restarts is re-ducked at
  each restart (the onset re-arms per reference rising edge), felt as a brief
  "re-crush". Candidate fix: suppress the onset re-arm while the envelope gate
  already sees strong near-end, now that the echo is controlled enough to trust it.
- Voice-mode band-pass corners (`AEC_VOICE_HP_HZ`/`AEC_VOICE_LP_HZ`) and the cap
  floor (`FD_CAP_BIN_LO`) are the tuning knobs; STT was fine at `[300,3400]` worn.
- The SDK `openai_realtime.py` sample (worktree `openai-realtime-duplex`) is now
  full-duplex AND wires voice mode: its Lua frame app calls
  `frame.microphone.aec(on)` / `frame.microphone.voice(on)` (the standalone
  setters, equivalent to the harness's `start{aec=,voice=}` fields — both live on
  `feat/aec`, `lua_microphone.c:1116,1137`), and the START_LISTENING payload
  carries the same three named bytes (gain/aec/voice). Worn-validated against
  s2s 2026-07-15 at its shipping vol-100 default: see the barge-in A/B below.

## PDM start-up "pop": snip the rail + warm-start the DC blocker (2026-07-15)

The PDM front-end slews to the ±full-scale rail for the first few ms after every
mic start - a loud click on the near-end feed. Characterized from cold-start
captures (`pop_zoom.py` / `pop_bound.py`, added here; run any `lfs_pcm_*` or
`duplex_mic_*` through them): the transient is a **decimator settling step**, not
noise - the first 4 samples are a zero pad, then the output ramps into the rail
and holds it for **~5-12 ms** (82-108 railed samples, polarity per mic), after
which it relaxes toward the steady ~1250-count DC bias over ~55-60 ms. The rail
window carries no recoverable audio; the DC-ramp tail past it is real audio and
must be kept. The `iir-bypass` on the front-end (kept deliberately - the AEC
needs the raw full-band mic and DC-removes downstream) is why the raw DC step
rails instead of being absorbed.

The driver already had a start-of-stream discard (`discard-duration-ms`), but its
default had been regressed **50 -> 0** (commit `15bc6a1`) to save latency, which
re-exposed the pop. Fix: set `discard-duration-ms = <20>` on the halo pdm node -
one `MIC_BLOCK_INTERVAL_MS` (20 ms) block in the DMA-drain path, clearing the
~12 ms rail with margin at ~20 ms of start-up latency. Also fixed a latent unit
bug in the DMA discard (`pdm_dma_callback`): `discard_samples` (all-channel)
was compared against `sets` (sample-times), so **stereo over-discarded ~2x**;
corrected with `* num_channels`. Bench-verified on the Halo unit (raw `lfs_pop_*`): the
delivered stream now starts on the settled signal (first sample ~700, no rail).

Snipping the raw rail left a faint (~-10 dBFS, ~3 ms) band-limited *tick* on the
AEC/voice feed: the residual ~700-count DC-ramp step, differentiated by the
zero-state DC blocker (`mic_dc_block`) and then rung by the 4th-order voice
band-pass. Fix: **warm-start** the DC blocker - on the first block after a mic
start, prime `xprev` to that block's first sample per channel so
`y[0] = x[0] - xprev + R*yprev = 0` (the blocker begins in DC steady state and
only tracks *changes*). Runs unconditionally whenever the AEC pipeline runs; no
band-pass priming needed since the blocker now feeds it ~0 at start-up. Worn on
Halo unit the voice-path onset drops from a +9869 burst to ±1-2 counts (flat
silence), with the discard firing (`pdm_discarded=320` = one mono block),
`pdm_dropped=0`, `mic_lost=0`, `resyncs=0`. `duplex_vad_test.py` gained a
`--seconds N` flag (clean auto-stop through the same teardown as Ctrl-C, so the
wav is saved) for unattended start-up captures.

### Follow-ups

- If the AEC's *own* onset ever ticks (the DC-blocker warm-start only addresses
  the blocker + band-pass), a ~5-10 ms fade-in on the first delivered block is
  the belt-and-suspenders backstop - not needed as of this session.
- Sub-block (<20 ms) discard precision would need partial-block delivery in the
  DMA path; one 20 ms block is the natural unit and was sufficient.

## Do we still need the client RMS barge gate? Server-VAD-alone A/B (2026-07-15)

Question: now that voice mode + the low-band cap control the echo (the
self-interruption spirals are gone), does the client mic-RMS barge confirmation
(`--barge-confirm-frames 2` / `--barge-rms 150`) still earn its keep, or can the
server VAD carry turn-taking + interruption on its own? Note this is a DIFFERENT
knob from the earlier "RMS flush: keep it" test, which disabled the flush
*entirely* (`--barge-rms 99999`); here the flush stays, only the *debounce* is
removed. Isolated the gate inside `duplex_vad_test.py` (same firmware / Lua /
s2s, so nothing else varies) by comparing the default gate against
`--barge-rms 0 --barge-confirm-frames 1` (flush on the first VAD trip — exactly
what the shipping SDK `openai_realtime.py` does). Both worn, the Halo unit vol-80,
aec+voice on, ~2.5 min, same barge script. Each barge classified by the near-end
AC-RMS in the recorded mic wav at that instant (`scratchpad/barge_rms.py`):
real barge = wearer's voice (≥150), false = echo residual that tripped Silero
while silent.

| metric | gate ON (150/2) | gate OFF (SDK-equiv) |
| --- | --- | --- |
| replies / server-cancels | 21 / 1 | 21 / 0 |
| barges cutting active playback | 5 | 12 |
| — REAL (near-end ≥150) | 5 / 5 | 11 / 12 |
| — FALSE (echo residual, gate blocks) | **0** | **1** — 5.3 s of reply killed at RMS 34.6 while silent |
| resyncs / pacing | 0, held | 0, held |

- **Server VAD alone is mostly fine.** 11/12 gate-OFF cuts were genuine and
  prompt; the server's OWN auto-interrupt barely fires (0-1 cancels) — the client
  backlog flush does the real work in BOTH modes (the server can't unsend the
  ~8 s of BLE-queued audio). The echo control has knocked the false-trip rate low.
- **But not clean.** Gate-OFF let ONE false cutoff through — Silero tripped on
  echo residual (34.6 RMS) and the immediate flush killed 5.3 s of a reply mid-
  sentence while the wearer was silent (~1 per 3 min). Gate-ON had zero (all 5
  cuts were real voice; lowest confirmed real barge 186 RMS, comfortably clear).
- **The gate's cost is ~nil.** The 2-read/150 confirm adds ~20 ms; genuine barges
  still cleared it, so real-barge responsiveness was identical.
- **Subjectively the two felt the SAME** (worn listening test) — the one false cutoff
  wasn't even perceived. So the gate is a cheap safety margin against a rare,
  low-salience failure, not a must-have for basic turn-taking.

Then ran the actual shipping SDK sample (`brilliant_sdk/openai-realtime-duplex`,
`openai_realtime.py`, its own vol-100 default, no RMS gate) against TWO backends:

- **vs s2s (Silero VAD), worn:** 21 turns, **10 barge-ins each matching a real
  spoken interruption** (down to a short "Stop." → 72 frames), **0 errors**,
  natural conversation, **no spiral**. Server VAD alone holds up here.
- **vs the real OpenAI `gpt-realtime` endpoint, worn:** started clean (real barges:
  "poem about roses", "poem about space") but then **fell into an echo-driven
  self-interruption spiral.** Once the assistant shifted to longer chatty sign-offs
  ("Whenever you're ready, feel free to jump back in. I'm here to help.") with
  nobody talking over it, its OWN voice bled through the AEC, whisper transcribed
  it as user turns (`You: whenever you're ready.`, `You: Just say the word.` — all
  echoes of the assistant's own phrases), OpenAI's server VAD (threshold 0.6 +
  `interrupt_response`) treated each as a barge, flushed the reply, and the model
  answered its own echoed transcript → self-sustaining loop. 8 flushes / 12 "user"
  turns, most of the back half echo. `run4_sdk_openai_233xxx.log`.

- **vs OpenAI at vol-80 (`--volume` flag added to the sample, default stays 100):
  it STILL spiralled.** Re-ran the exact scenario at the harness's quieter level to
  isolate the volume confound. Per-barge flushes shrank (~277 vs ~455 frames mean,
  the echo is quieter) but the runaway loop was identical — the tail is textbook:
  assistant "好的，我随时在，等你需要的时候" → `You: 等你需要的时候` (its OWN words
  transcribed as the user) → barge → another "OK I'll wait" reply → repeat. Also
  saw whisper's canonical silence-hallucination ("請不吝點贊訂閱轉發打賞...") fire
  barges while silent. `run5_sdk_openai_vol80_233xxx.log`.

**So the spiral is NOT volume-driven** — it reproduced at both 100 and 80. Root
cause: OpenAI's server VAD + whisper transcription trips on the post-AEC echo
residual (and hallucinates text from near-silence), transcribing the assistant's
own voice as user turns. Silero (s2s) shrugs the same residual off; OpenAI
interrupts on it. Lowering volume mitigates severity but does not fix it.

**Verdict (backend-dependent):**
- Instrumented harness / s2s: the RMS gate is cheap insurance (0 vs 1 false
  cutoff), not a hard requirement — server VAD alone is usable.
- **Shipping SDK sample against real OpenAI: server VAD alone is NOT enough — it
  spirals.** The client near-end confirmation (the RMS gate: require real mic
  energy ≥150 before honouring a flush) is exactly the fix — the echo residual
  (~34-155 AC-RMS, measured above) can't clear the gate, so it can't sustain the
  loop. Equivalent levers to try/stack: lower speaker volume, raise the VAD
  threshold (>0.6), or disable server `interrupt_response` and route ALL barges
  through the client-confirmed flush (the `onset_guard`-style path). Since OpenAI
  is the shipping target, the sample should adopt one of these before it ships as
  a real-OpenAI demo. Volume is confirmed NOT the fix (vol-80 spiralled too); the
  near-end confirmation is the robust lever.

**RMS gate + playback clock in the SDK sample (2026-07-16, worn-validated vs OpenAI):**
`openai_realtime.py` now stops the spiral. Two changes, in order of importance:

1. **Playback clock (the actual cure).** The gate must be applied only while a
   reply is playing, but "is a reply playing?" was keyed on the CLIENT queue /
   last-received-delta - which goes idle SECONDS before the device finishes the
   physical playout (generation is fast; the device dribbles the buffer out in
   real time). In that window the gate flipped OFF mid-playout and the reply's
   tail streamed to the server with no gate - the real leak path. Raising the RMS
   threshold could not touch it (the `[gate open]` telemetry proved every gate
   open was a real 500+ barge; the phantom turns had NO open - they came through
   gate-off). Fix: a `play_until` clock advanced by the audio duration actually
   sent to the device; playback counts as pending until it elapses + a device-
   buffer margin (`PLAYBACK_TAIL_S`). This kept the gate armed for the whole
   playout and turned a runaway spiral into ~1 isolated near-threshold trip.
2. **Gate that gates what the SERVER HEARS, not just the client flush.** While a
   reply plays, hold the mic closed to the server; open only when near-end AC-RMS
   clears `--barge-rms` for `--barge-confirm-frames` reads, back-filling a short
   onset buffer (no clip), re-closing after `--barge-hangover-ms`. Gating only the
   flush would not help (mic still forwarded -> echo still transcribed+answered:
   "keeps cutting itself off" becomes "keeps answering itself"). Energy, not the
   VAD `threshold`, is the separator: the echo IS speech-shaped so it scores HIGH
   on speech-probability; only the client sees clean near-end ENERGY. Threshold
   raised 150 -> **400** after telemetry showed real worn barges open at 418-1118
   while a stray echo/hallucination open landed ~337 (400 sits in that gap; costs
   the very softest barges, which is fine - barging over playback is deliberate).

Result (worn, the Halo unit): **vol-80** gate-400 - spiral gone, ~1 near-threshold trip;
**vol-100** (shipping default, louder echo) - CLEAN: 4 opens all real (647-833),
0 phantom turns, 0 spiral. **Regression vs s2s (Silero) at vol-100 with the same
default-on gate** - CLEAN: 5 opens all real (475-1700, none below 400 so no missed
barges), 13 normal turns, 0 errors despite s2s's different event model (no
response.created/done); the gate is redundant there but harmless (`--barge-rms 0`
for pure full duplex). The server VAD knobs (`--vad-threshold`,
`--vad-silence-ms`) never needed changing and stay exposed to tune/stack. Follow-up
lever if zero self-interrupts is ever required: `create_response:false` + client-
driven `response.create` gated on a higher confirm RMS, so a leaked phantom is
transcribed but never answered (no new turn-detection - the server still marks turn
boundaries). Not needed at vol-100.

Artifacts (this dir): `duplex_*_233311` (gate ON), `duplex_*_233614` (gate OFF),
`run3_sdk_sample_233xxx.log` (SDK vs s2s, clean), `run4_sdk_openai_233xxx.log`
(SDK vs OpenAI vol-100 spiral), `run5_sdk_openai_vol80_233xxx.log` (vol-80 spiral),
`run6..run7_*_gate*.log` (gate-only, still leaked via gate-off window),
`run8_*_playclock.log` (play clock, vol-80, ~1 trip),
`run9_*_vol100_gate400.log` (play clock + gate 400, vol-100, CLEAN),
`run10_*_s2s_vol100_gate400.log` (s2s regression, gate default-on, CLEAN),
`barge_rms.py` (the near-end-RMS barge classifier).
