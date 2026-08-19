/*
 * Host-side validation of audio_aec.c against the failure modes measured on
 * the Halo unit (2026-07-12): onset divergence burst, idle-dither noise injection,
 * pause/resume behaviour, underrun phase slips, plus baseline convergence.
 *
 * Mimics the device: reference fed in 20ms stereo int16 blocks (the I2S tap),
 * mic processed in 20ms mono int16 blocks. Echo = ref * synthetic IR with
 * 300-sample bulk delay, ~-30dB, plus -70dBFS mic noise.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <halo/audio_aec.h>
#include <max98357a_audio.h>

void max98357a_audio_set_tx_tap(max98357a_audio_tx_tap_t tap) { (void)tap; }

uint32_t host_uptime_ms; /* virtual clock for k_uptime_get_32() */

#define SR    16000
#define BLK   320 /* 20ms */
/* device-realistic echo depth (chained pair captures, Halo unit + dev kit,
 * 2026-07-13): the physical emission-to-capture delay is only ~1-2ms.
 * The AEC's one-block hold-back plus AEC_REF_LEAD place it at
 * d = 320 + IR_DELAY - REF_LEAD (~+158) in the window.
 */
#define IR_DELAY 30
#define IR_LEN   200

static float ir[IR_LEN];
static uint32_t rng = 0x12345678;

static float frand(void) /* uniform -1..1, deterministic */
{
	rng = rng * 1664525u + 1013904223u;
	return ((int32_t)rng >> 8) / 8388608.0f;
}

/* speech-ish reference: band-passed noise with syllabic (4Hz-ish) AM.
 * High-passed ~100Hz like the real speaker path (bone-conduction EQ), so
 * the reference carries no energy the mic's LF rumble could be
 * "explained" by - otherwise the filter legitimately cancels rumble and
 * the rumble-excluded scoring below mis-reads that as added residual.
 */
static float lp_state;
static float lp_state2[2];
static float hp_state[3];
static float env_state = 0.5f;
static float ref_sample(void)
{
	lp_state += 0.25f * (frand() - lp_state);
	env_state += 0.0004f * (0.5f + 0.5f * frand() - env_state);

	float v = lp_state * env_state * 0.5f; /* ~-20dBFS-ish when active */

	/* steepen the HF rolloff (~2kHz, +12dB/oct on top of the pole
	 * above): the real speaker path (bone-conduction transducer +
	 * spk_protect EQ) is speech-band concentrated - a 6dB/oct-only
	 * reference leaves unrealistically much echo above 4kHz
	 */
	for (int s = 0; s < 2; s++) {
		lp_state2[s] += 0.544f * (v - lp_state2[s]);
		v = lp_state2[s];
	}

	/* ~100Hz, 3 cascaded one-poles (~18dB/oct) - the real speaker path
	 * runs a proper biquad HPF EQ, so the reference reaching the tap has
	 * essentially no LF the filter could chase mic rumble with
	 */
	for (int s = 0; s < 3; s++) {
		hp_state[s] += 0.04f * (v - hp_state[s]);
		v -= hp_state[s];
	}
	return v;
}

/* Speech-like reference for the voice scenario: a pitch-pulse train
 * (wobbling f0 ~140-170Hz) through two formant-ish resonant bandpasses,
 * with syllabic (300ms) and utterance (1.5s/0.5s) gating. Unlike white
 * noise this is spectrally sparse, periodic (phase-predictable across
 * the 64ms window) and heavy-crested - the properties that broke the
 * device on a real assistant-voice clip while noise sat at the ceiling.
 */
static float sp_phase, sp_lp1, sp_f1a, sp_f1b, sp_f2a, sp_f2b;
static int sp_t;
static float speech_sample(void)
{
	sp_t++;

	float tsec = sp_t / 16000.0f;
	/* utterance gating: 1.5s speech, 0.5s silence */
	float ut = fmodf(tsec, 2.0f) < 1.5f ? 1.0f : 0.0f;
	/* syllabic gating: 300ms on, 120ms off */
	float syl = fmodf(tsec, 0.42f) < 0.3f ? 1.0f : 0.0f;
	float f0 = 155.0f + 25.0f * sinf(2.0f * 3.14159f * 0.8f * tsec);

	sp_phase += f0 / 16000.0f;
	float v = 0.0f;

	if (sp_phase >= 1.0f) {
		sp_phase -= 1.0f;
		v = 1.0f; /* glottal pulse */
	}
	/* "formants": two resonant paths made of HP'd LP pairs */
	sp_lp1 += 0.25f * (v - sp_lp1);
	sp_f1a += 0.17f * (sp_lp1 - sp_f1a);   /* ~500Hz-ish */
	sp_f1b += 0.17f * (sp_f1a - sp_f1b);
	sp_f2a += 0.45f * (sp_lp1 - sp_f2a);   /* ~1.5kHz-ish */
	sp_f2b += 0.45f * (sp_f2a - sp_f2b);

	float out = (2.5f * (sp_f1a - sp_f1b) + 1.5f * (sp_f2a - sp_f2b) +
		     0.6f * sp_lp1) * ut * syl;

	return out * 1.2f; /* RMS ~-20dBFS when voiced, crest ~12dB */
}

/* echo path state */
static float ref_hist[IR_DELAY + IR_LEN + BLK];

/* HF-dominated mic noise (device-realistic: the dev kit's update-domain
 * noise sits ~10dB above the echo full-band while the speech band is
 * ~20dB below it). Second-differenced white noise: +12dB/oct tilt, so
 * almost all its energy lands above the speech band. Excluded from
 * scoring like the rumble (it is known additive non-echo).
 */
static float hf_noise_amp;

static void make_ir(void)
{
	float g = 0.03f; /* ~-30dB path */

	for (int i = 0; i < IR_LEN; i++) {
		ir[i] = g * frand() * expf(-(float)i / 40.0f);
	}
}

/* Streaming 300-3400Hz scoring band (2x one-pole HP + 2x one-pole LP),
 * mirroring the device harness's band-passed scoring: the canceller by
 * design does not operate below 300Hz (rumble immunity) or above the
 * speech band, so ERLE is only meaningful inside it.
 */
struct bandf { float lf1, lf2, lp[2]; };

static float bandf_run(struct bandf *f, float v)
{
	f->lf1 += 0.1111f * (v - f->lf1);
	v -= f->lf1;
	f->lf2 += 0.1111f * (v - f->lf2);
	v -= f->lf2;
	for (int s = 0; s < 2; s++) {
		f->lp[s] += 0.737f * (v - f->lp[s]);
		v = f->lp[s];
	}
	return v;
}

static struct bandf sc_in, sc_out;

/* one 20ms step: generate ref block (or silence/dither), feed the tap as
 * stereo, build mic = echo + noise (+ near), run audio_aec_process.
 * mode: 0 = no tap call at all, 1 = dither, 2 = zeros, 3 = active
 * Returns output block RMS (dBFS); in/out powers accumulated by caller.
 */
struct blkstat { float in_rms, out_rms, near_rms; };

/* near-end (wearer) speech injected into the mic when > 0: scales
 * speech_sample(), which at 1.2 sits ~-20dBFS voiced
 */
static float near_amp;

static int16_t deferred_tap[2 * BLK];
static int have_deferred;

/* producer-side tap latency (ms): the DMA-completion callback firing
 * this long after the block physically finished emitting. Biases the
 * emission-epoch observations; the min-estimator absorbs a constant,
 * but a PER-REPLY change relocates each re-established epoch.
 */
static int g_tap_late_ms;

/* upstream capture loss: the world advances (tap fed, echo emitted,
 * virtual clock ticks) but the consumer never sees this mic block -
 * models the PDM driver losing a block under load (queue-full drop,
 * FIFO overflow clear). Scoring state stays on the last PROCESSED
 * block, matching the AEC's held block.
 */
static int g_drop_mic;

/* defer_tap: hold this block's tap back and deliver it (before the next
 * block's tap) on the following step - models the I2S tap firing late
 * relative to the mic consumer (a 20ms underrun then catch-up).
 * mic_late_ms: consumer scheduling latency - the mic block is processed
 * this long after the virtual capture/feed instant (models the mic
 * thread being held off by LC3 encode / other load).
 */
static struct blkstat step_ex(int mode, int defer_tap, int mic_late_ms)
{
	float refblk[BLK];
	int16_t tap[2 * BLK];
	int16_t mic[BLK];
	struct blkstat st;

	host_uptime_ms += 20;

	for (int i = 0; i < BLK; i++) {
		float v = 0.0f;

		if (mode == 3) {
			v = ref_sample();
		} else if (mode == 4) {
			v = speech_sample();
		} else if (mode == 1) {
			v = ((int)(rng = rng * 1664525u + 1013904223u) & 1)
				? (1.0f / 32768.0f) : (-1.0f / 32768.0f);
		}
		refblk[i] = v;
		tap[2 * i] = tap[2 * i + 1] = (int16_t)(v * 32767.0f);
	}
	host_uptime_ms += g_tap_late_ms;
	if (have_deferred) {
		audio_aec_feed_reference(deferred_tap, 2 * BLK, SR, 2);
		have_deferred = 0;
	}
	if (mode != 0) {
		if (defer_tap) {
			memcpy(deferred_tap, tap, sizeof(tap));
			have_deferred = 1;
		} else {
			audio_aec_feed_reference(tap, 2 * BLK, SR, 2);
		}
	}
	host_uptime_ms -= g_tap_late_ms;

	/* echo through the IR (ref history includes this block) */
	memmove(ref_hist, ref_hist + BLK,
		(IR_DELAY + IR_LEN) * sizeof(float));
	memcpy(ref_hist + IR_DELAY + IR_LEN, refblk, BLK * sizeof(float));

	float pin = 0.0f, pout = 0.0f, pnear = 0.0f;
	float rum[BLK];
	static float rumble_lp, rumble_lp2, rumble_lp3;
	static float hfw1, hfw2;

	for (int i = 0; i < BLK; i++) {
		float echo = 0.0f;
		/* sample i of this block sits at index IR_DELAY+IR_LEN+i
		 * minus... convolve: echo[n] = sum_k ir[k]*ref[n - IR_DELAY - k]
		 */
		for (int k = 0; k < IR_LEN; k++) {
			echo += ir[k] * ref_hist[IR_LEN + i - k];
		}
		/* device realism, per the Halo unit measurements:
		 * - H2-style distortion ~-16dB relative to the linear echo
		 *   (ref-correlated but not linearly explainable)
		 * - constant ~-26dBFS sub-100Hz rumble in every capture,
		 *   entirely uncorrelated with the reference
		 */
		echo += 30.0f * echo * fabsf(echo);
		/* narrowband NOISE with a steep (18dB/oct) skirt, matching the
		 * device: >90% of energy <100Hz and ~29dB down by 100-300Hz.
		 * Not a sine - a sine is phase-predictable over the filter's
		 * 64ms window from any residual LF reference energy.
		 */
		rumble_lp += 0.012f * (frand() - rumble_lp);
		rumble_lp2 += 0.012f * (rumble_lp - rumble_lp2);
		rumble_lp3 += 0.012f * (rumble_lp2 - rumble_lp3);
		float rumble = 5.0f * rumble_lp3;
		float noise = 3e-4f * frand(); /* ~-70dBFS */
		float hfw = frand();
		float hfn = hf_noise_amp * (hfw - 2.0f * hfw1 + hfw2);

		hfw2 = hfw1;
		hfw1 = hfw;

		float near = (near_amp > 0.0f) ? near_amp * speech_sample()
					       : 0.0f;
		float d = echo + rumble + hfn + noise + near;

		pnear += near * near;
		rum[i] = rumble + hfn;

		if (d > 1.0f) d = 1.0f;
		if (d < -1.0f) d = -1.0f;
		mic[i] = (int16_t)(d * 32767.0f);
		/* score in the 300-3400 band without the known additive
		 * noise: ERLE measures cancellation in the band the
		 * canceller is designed for (mirrors the device scoring)
		 */
		float si = bandf_run(&sc_in, d - rum[i]);

		pin += si * si;
	}

	if (g_drop_mic) {
		struct blkstat zs = { 0.0f, 0.0f, 0.0f };

		return zs;
	}

	/* the consumer never observes time before the tap callback that
	 * preceded it (monotonic clock); without this, a future-dated feed
	 * stamp wraps the unsigned hangover age and fakes a disengage
	 */
	int adv = (mic_late_ms > g_tap_late_ms) ? mic_late_ms : g_tap_late_ms;

	host_uptime_ms += adv;
	audio_aec_process(mic, BLK, SR, 1);
	host_uptime_ms -= adv;

	/* the AEC's one-block hold-back delays its output by one block, so
	 * out(b) is the cancelled in(b-1): score against the previous
	 * block's input (and its known additive noise). The FDAF build's
	 * residual suppressor adds a further SUP_D-sample group delay
	 * (its causal gain kernel), so the known-noise expectation must
	 * shift by SUP_D too - the rumble dominates the mic and any
	 * misalignment reads as huge fake excess.
	 */
	static float prev_pin;
	static float prev_rum[BLK];

#ifdef CONFIG_HALO_AUDIO_AEC_FDAF
#define SUP_D 128
	static float rum2[BLK]; /* block b-2's known noise */

	for (int i = 0; i < BLK; i++) {
		float expect = (i < SUP_D) ? rum2[BLK - SUP_D + i]
					   : prev_rum[i - SUP_D];
		float o = bandf_run(&sc_out, mic[i] / 32768.0f - expect);

		pout += o * o;
	}
	memcpy(rum2, prev_rum, sizeof(rum2));
#else
	for (int i = 0; i < BLK; i++) {
		float o = bandf_run(&sc_out, mic[i] / 32768.0f - prev_rum[i]);

		pout += o * o;
	}
#endif
	memcpy(prev_rum, rum, sizeof(rum));
	/* near_rms reports the same (previous) block as in_rms/out_rms */
	static float prev_pnear;

	st.in_rms = 10.0f * log10f(prev_pin / BLK + 1e-15f);
	st.out_rms = 10.0f * log10f(pout / BLK + 1e-15f);
	st.near_rms = 10.0f * log10f(prev_pnear / BLK + 1e-15f);
	prev_pin = pin;
	prev_pnear = pnear;
	return st;
}

static struct blkstat step(int mode)
{
	return step_ex(mode, 0, 0);
}

static struct blkstat step_lat(int mode, int mic_late_ms)
{
	return step_ex(mode, 0, mic_late_ms);
}

/* backlog flush: deliver n captured-but-delayed mic blocks in a burst.
 * The consumer credits n blocks of mic_total while the wall clock barely
 * advances (1ms/block, kept moving so the monotonic-clock guard holds),
 * so the capture-epoch observation (now - mic_total/16) drops by ~one
 * block per flushed block. Models the mic thread catching up after a
 * scheduling stall the late-floor backstop had already misread as loss
 * (and slid the epoch later for). The reference ring keeps its own
 * real-time timeline (no tap fed, clock ~frozen), so the served window
 * jumps ahead of the write head - the walk-off. Blocks are silence;
 * scoring resumes on the speech steps that follow.
 */
static void flush_mic(int n)
{
	int16_t mic[BLK] = {0};

	for (int k = 0; k < n; k++) {
		host_uptime_ms += 1;
		audio_aec_process(mic, BLK, SR, 1);
	}
}

static int verbose;

static int failures;

static void check(const char *name, int ok, const char *detail)
{
	printf("%-44s %s  %s\n", name, ok ? "PASS" : "FAIL", detail);
	failures += !ok;
}

int main(int argc, char **argv)
{
	char buf[128];

	verbose = argc > 1;
	make_ir();

	/* --- 1. baseline convergence: 8s active, ERLE over the last 2s --- */
	audio_aec_enable(true);
	{
		double pi = 0, po = 0;

		for (int b = 0; b < 400; b++) {
			struct blkstat s = step(3);

			if (verbose && (b % 20) == 0) {
				printf("  t1 b%3d in %6.1f out %6.1f\n",
				       b, s.in_rms, s.out_rms);
			}
			if (b >= 300) {
				pi += pow(10, s.in_rms / 10);
				po += pow(10, s.out_rms / 10);
			}
		}
		double erle = 10 * log10(pi / po);

#ifdef CONFIG_HALO_AUDIO_AEC_FDAF
		snprintf(buf, sizeof(buf), "ERLE %.1f dB (want > 14)", erle);
		check("convergence on synthetic echo (H2 ceiling)", erle > 14, buf);
#else
		snprintf(buf, sizeof(buf), "ERLE %.1f dB (want > 10)", erle);
		check("convergence on synthetic echo (H2 ceiling)", erle > 10, buf);
#endif
	}

	/* --- 2. onset burst: fresh filter, 2s idle then playback ------- */
	audio_aec_enable(true);
	{
		float worst = -1000.0f;
		float in_prev = -1000.0f;
		int worst_b = -1;

		for (int b = 0; b < 100; b++) {
			step(0); /* idle, no tap */
		}
		double pi = 0, po = 0;

		for (int b = 0; b < 200; b++) { /* 4s of playback */
			struct blkstat s = step(3);
			/* excess vs the louder of the two neighbouring input
			 * blocks: the suppressor's D-sample delay smears the
			 * output across a block boundary, so a falling
			 * envelope otherwise reads its own slope (~2dB) as
			 * fake excess. Real bursts tower over BOTH blocks.
			 */
			float in_hi = (in_prev > s.in_rms) ? in_prev : s.in_rms;
			float excess = s.out_rms - in_hi;

			in_prev = s.in_rms;

			/* relative excess on a sub-audible block (echo not
			 * yet arrived, mic near the noise floor, the
			 * stimulus envelope still ramping) is not a burst -
			 * a few dB on content below -60dBFS is nothing,
			 * while real divergence bursts ride ON the echo
			 * (-40s dBFS). b 0-2 are the engage transition: the hold-back's inserted
			 * silence block and the suppressor's D-sample delay
			 * stream transit the scorer's shifted-noise
			 * expectation, which is undefined across that
			 * discontinuity (verified: the excess is rumble
			 * misalignment, not filter output). Real divergence
			 * bursts run hundreds of ms and b>=3 catches them.
			 */
			if (b < 3 || s.in_rms < -60.0f) {
				continue;
			}
			if (excess > worst) {
				worst = excess;
				worst_b = b;
			}
			if (b >= 100) {
				pi += pow(10, s.in_rms / 10);
				po += pow(10, s.out_rms / 10);
			}
		}
		double erle = 10 * log10(pi / po);

		snprintf(buf, sizeof(buf),
			 "worst out-in %.1f dB@b%d (want < 1), ERLE(3-4s) %.1f dB (want > 6)",
			 worst, worst_b, erle);
		/* the ERLE floor guards against the idle-lead-in deadlock: an
		 * AEC that silently does nothing also never bursts
		 */
		check("onset after idle: no burst AND converges",
		      worst < 1.0f && erle > 6, buf);
	}

	/* --- 3. idle dither: no noise injection ------------------------ */
	audio_aec_enable(true);
	{
		/* converge first, then 10s of dithered 'paused' session */
		for (int b = 0; b < 300; b++) {
			step(3);
		}
		float worst = -1000.0f;

		for (int b = 0; b < 500; b++) {
			struct blkstat s = step(1);

			if (b > 25) { /* skip echo tail */
				float excess = s.out_rms - s.in_rms;

				if (excess > worst) {
					worst = excess;
				}
			}
		}
		snprintf(buf, sizeof(buf),
			 "worst out-in %.1f dB (want < 1)", worst);
		check("no injection on dithered idle session", worst < 1.0f, buf);
	}

	/* --- 4. pause/resume: no burst, ERLE recovers ------------------ */
	{
		for (int b = 0; b < 100; b++) {
			step(2); /* hard zeros (paused) */
		}
		float worst = -1000.0f;
		double pi = 0, po = 0;

		for (int b = 0; b < 200; b++) {
			struct blkstat s = step(3);
			float excess = s.out_rms - s.in_rms;

			/* only audible bursts count: a few dB of filter
			 * gradient-noise against near-silence (-70dBFS) is
			 * far below the speech and not a defect
			 */
			if (excess > worst && s.out_rms > -55.0f) {
				worst = excess;
			}
			if (b >= 100) {
				pi += pow(10, s.in_rms / 10);
				po += pow(10, s.out_rms / 10);
			}
		}
		double erle = 10 * log10(pi / po);

		snprintf(buf, sizeof(buf),
			 "worst out-in %.1f dB, ERLE(2nd sec+) %.1f dB", worst, erle);
		/* a ~2dB single-block excess right at resume is the stale
		 * zero-padded window flushing through y - at ~30dB below the
		 * speech it is inaudible. The guard here is against the
		 * 10-20dB divergence bursts the old code produced.
		 */
		check("clean resume after pause", worst < 3.0f && erle > 8, buf);
	}

	/* --- 5. recovery from a consumer-vs-ISR phase slip ------------- */
	audio_aec_enable(true);
	{
		/* one late tap mid-run: the consumer pads a block, the
		 * catch-up burst grows the backlog, and the timeline gains
		 * an inserted block of silence. The filter must re-adapt
		 * within a couple of seconds. (Real slips come from crystal
		 * drift walking the thread/ISR phase through a block
		 * boundary - minutes apart, not periodic.)
		 */
		double pi = 0, po = 0;

		for (int b = 0; b < 400; b++) {
			struct blkstat s = step_ex(3, b == 200, 0);

			if (b >= 300) { /* 2s after the slip */
				pi += pow(10, s.in_rms / 10);
				po += pow(10, s.out_rms / 10);
			}
		}
		double erle = 10 * log10(pi / po);

		snprintf(buf, sizeof(buf), "ERLE %.1f dB (want > 6)", erle);
		check("recovers from a single ref phase slip", erle > 6, buf);
	}

	/* --- 6. post-freeze scheduling luck must not move the window ---- */
	audio_aec_enable(true);
	{
		struct audio_aec_stats st0, st1;
		size_t taps;

		/* fresh timelines (mic-session restart + feed gap), then
		 * converge with a constant 12ms consumer scheduling latency:
		 * the epochs establish with that latency baked in, and
		 * freeze 2s later
		 */
		host_uptime_ms += 300;
		for (int b = 0; b < 400; b++) {
			step_lat(3, 12);
		}
		audio_aec_snapshot(&st0, &taps);
		/* one lucky low-latency block. Unfrozen, this refines the
		 * mic epoch by 12ms = 192 samples - over the 160-sample
		 * drift threshold, so the next consume resyncs: history
		 * wipe plus a shifted re-anchor, and ERLE collapses while
		 * the filter refits (the anchor-churn measured on the Halo unit
		 * under LC3 load). Frozen, it must be counted, suppressed,
		 * and cause no resync.
		 */
		step_lat(3, 0);

		double pi = 0, po = 0;

		for (int b = 0; b < 50; b++) {
			struct blkstat s = step_lat(3, 12);

			pi += pow(10, s.in_rms / 10);
			po += pow(10, s.out_rms / 10);
		}
		audio_aec_snapshot(&st1, &taps);

		double erle = 10 * log10(pi / po);

		snprintf(buf, sizeof(buf),
			 "ERLE %.1f dB (want > 8), suppressed %u (%u ms), resyncs +%u",
			 erle, st1.cap_late - st0.cap_late,
			 st1.cap_late_ms - st0.cap_late_ms,
			 st1.resyncs - st0.resyncs);
		check("epoch freeze holds window through jitter",
		      erle > 8 && st1.cap_late > st0.cap_late &&
		      st1.resyncs == st0.resyncs, buf);
	}

	/* --- 7. paired capture reads the true echo delay ---------------- */
	{
		const int16_t *pm;
		const int16_t *pr;
		size_t ptaps = 0, n;
		int lag = -1;

		audio_aec_pair_request();
		step_lat(3, 12);
		n = audio_aec_pair_read(&pm, &pr, &ptaps);
		if (n > 0) {
			/* matched filter with the KNOWN echo IR: for each
			 * candidate window base b, predict the echo from
			 * the captured ref window (mic[i] pairs with
			 * ref[i + taps - 1 - b - k] through ir[k]) and
			 * correlate with the first-differenced mic - the
			 * peak b is the echo's base delay in tap
			 * coordinates, exactly the window placement
			 */
			double best = -1.0;

			for (size_t b = 0; b + IR_LEN < ptaps; b++) {
				double c = 0.0;

				for (size_t i = 1; i < n; i++) {
					double e0 = 0.0, e1 = 0.0;

					for (int k = 0; k < IR_LEN; k++) {
						e1 += ir[k] * pr[i + ptaps - 1 - b - k];
						e0 += ir[k] * pr[i + ptaps - 2 - b - k];
					}
					c += (double)(pm[i] - pm[i - 1]) * (e1 - e0);
				}
				if (fabs(c) > best) {
					best = fabs(c);
					lag = (int)b;
				}
			}
		}

		/* ground truth: one-block hold-back + IR_DELAY, re-centred
		 * by AEC_REF_LEAD (192), plus the constant 12ms
		 * consumer-latency bias baked into the frozen mic epoch
		 */
		int expect = 320 + IR_DELAY - 192 + 16 * 12;

		snprintf(buf, sizeof(buf),
			 "pair lag %d taps, expected %d (want within 8)",
			 lag, expect);
		check("paired capture reads true window placement",
		      n > 0 && abs(lag - expect) <= 8, buf);
	}

	/* --- 8. HF-dominated mic noise must not wreck in-band ERLE ------ */
	audio_aec_enable(true);
	{
		/* full-band, this noise puts the update-domain gradient
		 * ~10dB above the echo and NLMS weight noise nets ~0 ERLE
		 * (the dev kit steady-state failure); the band-limited
		 * update must hold in-band cancellation regardless
		 */
		hf_noise_amp = 0.009f;

		double pi = 0, po = 0;

		for (int b = 0; b < 400; b++) {
			struct blkstat s = step(3);

			if (b >= 300) {
				pi += pow(10, s.in_rms / 10);
				po += pow(10, s.out_rms / 10);
			}
		}
		hf_noise_amp = 0.0f;

		double erle = 10 * log10(pi / po);

		snprintf(buf, sizeof(buf), "ERLE %.1f dB (want > 5)", erle);
		check("in-band ERLE despite HF mic noise", erle > 5, buf);
	}

	/* --- 9. speech-like stimulus: sparse, periodic, gappy ----------- */
	audio_aec_enable(true);
	{
		/* reproduces the on-device assistant-voice failure: noise
		 * converged at the ceiling while a real voice clip left
		 * ERLE ~0 with w_norm2 GROWING (2.4 vs the true ~1) - the
		 * update built phantom taps from spectrally-sparse periodic
		 * content. Score voiced blocks only.
		 */
		struct audio_aec_stats st;
		size_t taps;
		double pi = 0, po = 0;

		for (int b = 0; b < 700; b++) { /* 14s */
			struct blkstat s = step(4);

			if (b >= 250 && s.in_rms > -55.0f) {
				pi += pow(10, s.in_rms / 10);
				po += pow(10, s.out_rms / 10);
			}
		}
		audio_aec_snapshot(&st, &taps);

		double erle = 10 * log10(pi / po);

#ifdef CONFIG_HALO_AUDIO_AEC_FDAF
		/* the per-bin build exists precisely for this scenario:
		 * hold it to the coherence-bound region, not the
		 * time-domain plateau
		 */
		snprintf(buf, sizeof(buf),
			 "voiced ERLE %.1f dB (want > 8), ||w||^2 %.2f (want < 1.0)",
			 erle, st.w_norm2);
		check("speech-like stimulus converges, w bounded",
		      erle > 8 && st.w_norm2 < 1.0, buf);
#else
		/* interim time-domain plateau (leak + PNLMS, no preemph):
		 * plain NLMS scores 1.5 here with w growing 250x; closing
		 * the remaining gap to the measured 6-10dB coherence bound
		 * is the subband/FDAF stage's job
		 */
		snprintf(buf, sizeof(buf),
			 "voiced ERLE %.1f dB (want > 3), ||w||^2 %.2f (want < 1.0)",
			 erle, st.w_norm2);
		check("speech-like stimulus converges, w bounded",
		      erle > 3 && st.w_norm2 < 1.0, buf);
#endif
	}

	/* --- 10. speaker-idle bypass: zero latency, bit-exact, clean resume */
	audio_aec_enable(true);
	{
		/* converge, then let the speaker stop for well over the
		 * activity hangover
		 */
		for (int b = 0; b < 300; b++) {
			step(3);
		}
		for (int b = 0; b < 25; b++) {
			step(0); /* 500ms idle - no tap feeds at all */
		}

		/* a mic-only block (e.g. AAD wakeup) must come back untouched
		 * in ITS OWN slot: bit-exact passthrough = zero added latency,
		 * no hold-back, no filtering
		 */
		int16_t blk[BLK], orig[BLK];

		for (int i = 0; i < BLK; i++) {
			blk[i] = (int16_t)(3000.0f *
				 sinf(2.0f * 3.14159f * 440.0f * i / 16000.0f));
			orig[i] = blk[i];
		}
		host_uptime_ms += 20;
		audio_aec_process(blk, BLK, SR, 1);

		int exact = memcmp(blk, orig, sizeof(blk)) == 0;

		/* playback resumes: hold-back re-engages without a burst and
		 * cancellation recovers
		 */
		float worst = -1000.0f;
		float in_prev10 = -1000.0f;
		int worst_b10 = -1;
		double pi = 0, po = 0;

		for (int b = 0; b < 200; b++) {
			struct blkstat s = step(3);
			/* excess vs the louder neighbour (see scenario 2) */
			float in_hi = (in_prev10 > s.in_rms) ? in_prev10
							     : s.in_rms;
			float excess = s.out_rms - in_hi;

			in_prev10 = s.in_rms;

			/* b 0-2 are the engage transition: the intended
			 * one-block silence insertion (plus the suppressor's
			 * D-sample delay stream starting up) shifts the
			 * output timeline, so the harness's delayed
			 * rumble-exclusion subtracts the wrong content there
			 * and mis-scores it (verified: y is at -78dBFS on
			 * those blocks - the filter outputs nothing)
			 */
			if (b >= 3 && s.in_rms > -70.0f && excess > worst) {
				worst = excess;
				worst_b10 = b;
			}
			if (b >= 100) {
				pi += pow(10, s.in_rms / 10);
				po += pow(10, s.out_rms / 10);
			}
		}

		double erle = 10 * log10(pi / po);

		snprintf(buf, sizeof(buf),
			 "passthrough %s, worst out-in %.1f dB@b%d, ERLE %.1f dB",
			 exact ? "bit-exact" : "MODIFIED", worst, worst_b10, erle);
		check("speaker-idle bypass + clean re-engage",
		      exact && worst < 1.0f && erle > 6, buf);
	}

	/* --- 11. cold-onset residual: the barge-in gap ------------------ */
	audio_aec_enable(true);
	{
		/* a FRESH filter (only enable() wipes W) at playback onset:
		 * this is the first assistant reply of a session, whose
		 * residual used to pass nearly uncancelled for ~3s and
		 * false-trigger interruptions. The FDAF build's hot-mu
		 * schedule + residual suppressor must cancel from the first
		 * blocks; the TD build has neither and is only held to its
		 * historical onset behaviour.
		 */
		double pi = 0, po = 0;

		for (int b = 0; b < 100; b++) {
			step(0);
		}
		for (int b = 0; b < 75; b++) { /* first 1.5s of playback */
			struct blkstat s = step(4); /* speech: the real case */

			if (b >= 3 && s.in_rms > -55.0f) {
				pi += pow(10, s.in_rms / 10);
				po += pow(10, s.out_rms / 10);
			}
		}
		double erle = 10 * log10(pi / po);

#ifdef CONFIG_HALO_AUDIO_AEC_FDAF
		/* old FDAF (0.5s soft-start, no suppressor): 1.3dB; hot-mu
		 * without the history-fill guard: -3.3dB. The bar sits well
		 * above both while allowing for the guard's silent 120ms.
		 */
		snprintf(buf, sizeof(buf), "ERLE(0-1.5s) %.1f dB (want > 6)",
			 erle);
		check("cold-onset residual cancelled (barge-in)", erle > 6, buf);
#else
		snprintf(buf, sizeof(buf), "ERLE(0-1.5s) %.1f dB (want > 2)",
			 erle);
		check("cold-onset residual cancelled (barge-in)", erle > 2, buf);
#endif
	}

	/* --- 12. warm re-engage: no soft-start cost on later replies ---- */
	audio_aec_enable(true);
	{
		/* W converged (fresh filter, 6s of noise - scenario 11's
		 * speech W would otherwise leak in), speaker idle past the
		 * hangover (every reply gap in a real conversation), then
		 * the next reply starts: the filter re-engages WARM - the
		 * cold-start schedule must not re-arm, and cancellation
		 * must be immediate
		 */
		for (int b = 0; b < 300; b++) {
			step(3);
		}
		for (int b = 0; b < 30; b++) {
			step(0); /* 600ms idle, > the 120ms hangover */
		}

		double pi = 0, po = 0;

		for (int b = 0; b < 50; b++) { /* first 1s of the reply */
			struct blkstat s = step(3);

			if (b >= 3 && s.in_rms > -55.0f) {
				pi += pow(10, s.in_rms / 10);
				po += pow(10, s.out_rms / 10);
			}
		}
		double erle = 10 * log10(pi / po);

#ifdef CONFIG_HALO_AUDIO_AEC_FDAF
		snprintf(buf, sizeof(buf), "ERLE(0-1s) %.1f dB (want > 12)",
			 erle);
		check("warm re-engage cancels immediately", erle > 12, buf);
#else
		/* the TD build re-fills its cleared ref history for ~64ms
		 * of uncancelled passthrough at re-engage, which dominates
		 * this energy ratio; it has no suppressor to cover that
		 * (FDAF's persisted Sy/Se prior floors those blocks) - hold
		 * it to warm-W retention only
		 */
		snprintf(buf, sizeof(buf), "ERLE(0-1s) %.1f dB (want > 4)",
			 erle);
		check("warm re-engage cancels immediately", erle > 4, buf);
#endif
	}

	/* --- 13. double-talk: the suppressor must not eat the wearer ---- */
	audio_aec_enable(true);
	{
		/* far-end noise playing, wearer speaking over it ~5dB above
		 * the echo: the whole point of barge-in. Ideal output keeps
		 * the near-end untouched (out ~= in minus the cancelled
		 * echo); a suppressor that ducks near-end-dominated blocks
		 * pushes out well below in. Median over near-active blocks.
		 */
		for (int b = 0; b < 300; b++) {
			step(3); /* converge first */
		}
		near_amp = 0.6f; /* ~-25dBFS voiced */

		float exc[300];
		int nexc = 0;

		for (int b = 0; b < 300; b++) {
			struct blkstat s = step(3);

			if (b >= 3 && s.near_rms > -35.0f) {
				exc[nexc++] = s.out_rms - s.in_rms;
			}
		}
		near_amp = 0.0f;

		/* median via insertion sort (n <= 300) */
		for (int i = 1; i < nexc; i++) {
			float v = exc[i];
			int j = i - 1;

			while (j >= 0 && exc[j] > v) {
				exc[j + 1] = exc[j];
				j--;
			}
			exc[j + 1] = v;
		}

		float med = nexc ? exc[nexc / 2] : -1000.0f;

		snprintf(buf, sizeof(buf),
			 "median out-in %.1f dB over %d near-active blocks "
			 "(want > -3)", med, nexc);
		check("double-talk: near-end speech preserved",
		      nexc > 50 && med > -3.0f, buf);
	}

	/* --- 14. reply-cycle duplex pattern: the live-loop case --------- */
	{
		/* The BLE duplex session on the Halo unit (2026-07-12) measured
		 * ~0 dB in-band cancellation across 16 reply spans while the
		 * single-span bench cancelled the same voice at 15 dB in-band
		 * - zero resyncs, converged w, all bookkeeping content. The
		 * difference is the REPLY CYCLE: every >40ms feed gap wipes
		 * the emission epoch, disengages/re-engages the hold-back,
		 * fences the ring, and re-anchors. This scenario runs that
		 * cycle 8 times per recipe with speech and per-reply timing
		 * stress, scoring in-band ERLE per reply. Recipes:
		 *   clean:      identical timing every reply (scenario-12ish)
		 *   tap-jitter: per-reply DMA-callback latency 0..5ms - each
		 *               re-established emission epoch lands shifted
		 *   mic-jitter: per-reply consumer latency (control: anchors
		 *               are index-arithmetic, this must NOT matter)
		 *   midgap:     a 60ms feed starve inside every reply
		 *   combo:      all of the above
		 *
		 * Gaps are ZERO-FED (mode 2), modelling the driver's
		 * silence-feed: while a speaker session is open the MAX98357A
		 * callback clocks zero blocks through any queue drain, so the
		 * tap never stops and the emission epoch is established once
		 * per session. Build with -DSC14_TRUE_GAPS for the pre-fix
		 * driver (feed stops at every reply gap): tap-jitter and
		 * combo MUST then fail - each reply-gap epoch
		 * re-establishment inherits that reply's callback
		 * latency/quantization offset, the anchor lands shifted, the
		 * per-bin phase rotates, and the misaligned residual reads
		 * as double-talk so adaptation never recovers the reply.
		 * That is the live BLE-duplex failure measured on the Halo unit
		 * (2026-07-12): in-band ~0dB vs 15dB single-span bench.
		 */
#ifdef SC14_TRUE_GAPS
#define SC14_GAP 0
#else
#define SC14_GAP 2
#endif
		static const struct {
			const char *name;
			int tap_late[8];
			int mic_late[8];
			int midgap;
		} rec[] = {
			{ "clean",      {0}, {0}, 0 },
			{ "tap-jitter", {0, 2, 5, 1, 4, 0, 3, 5}, {0}, 0 },
			{ "mic-jitter", {0}, {0, 8, 15, 3, 12, 0, 6, 10}, 0 },
			{ "midgap",     {0}, {0}, 1 },
			{ "combo",      {0, 2, 5, 1, 4, 0, 3, 5},
					{0, 8, 15, 3, 12, 0, 6, 10}, 1 },
		};

		for (unsigned r = 0; r < sizeof(rec) / sizeof(rec[0]); r++) {
			audio_aec_enable(true); /* fresh W per recipe */
			for (int b = 0; b < 150; b++) {
				step(4); /* 3s initial convergence, speech */
			}

			float cyc[8];
			double worst = 1e9, mean = 0.0;

			for (int k = 0; k < 8; k++) {
				g_tap_late_ms = rec[r].tap_late[k];
				for (int b = 0; b < 50; b++) {
					step(SC14_GAP); /* 1s reply gap */
				}

				double pi = 0, po = 0;

				for (int b = 0; b < 125; b++) { /* 2.5s */
					if (rec[r].midgap && b >= 60 &&
					    b < 63) {
						step(SC14_GAP);
						continue;
					}
					struct blkstat s = step_ex(4, 0,
						rec[r].mic_late[k]);

					if (b >= 5 && s.in_rms > -55.0f) {
						pi += pow(10, s.in_rms / 10);
						po += pow(10, s.out_rms / 10);
					}
				}
				cyc[k] = (float)(10 * log10(pi / (po + 1e-30)));
				if (k >= 2) {
					mean += cyc[k];
					if (cyc[k] < worst) {
						worst = cyc[k];
					}
				}
			}
			g_tap_late_ms = 0;
			mean /= 6.0;

			struct audio_aec_stats st;
			size_t ntaps;

			audio_aec_snapshot(&st, &ntaps);
			snprintf(buf, sizeof(buf),
				 "%-10s per-reply ERLE %.1f %.1f %.1f %.1f "
				 "%.1f %.1f %.1f %.1f, mean(3..8) %.1f dB, "
				 "worst %.1f, resyncs %u",
				 rec[r].name, cyc[0], cyc[1], cyc[2], cyc[3],
				 cyc[4], cyc[5], cyc[6], cyc[7], mean, worst,
				 st.resyncs);
#ifdef CONFIG_HALO_AUDIO_AEC_FDAF
			check("reply cycles hold cancellation",
			      mean > 8.0 && worst > 5.0, buf);
#else
			check("reply cycles hold cancellation",
			      mean > 2.0 && worst > 0.0, buf);
#endif
		}
	}

	/* --- 15. capture losses: dropped mic blocks --------------------- */
	{
		/* The 2026-07-13 live-duplex death at ~100s: under BLE+LC3
		 * load the PDM layer loses capture samples (FIFO overflow
		 * clears, queue-full drops). The reference window is
		 * anchored by CUMULATIVE SEEN mic samples - and so are both
		 * operands of the drift check - so every unaccounted loss
		 * walks the window off the reference ring at the loss rate:
		 * margin grows monotonically (Halo unit measured ~36
		 * samples/s, margin_max 7019), the ring goes all-pad, the
		 * linear filter AND the suppressor die, and not one resync
		 * is counted. Two fixes under test:
		 *   (a) reported losses (audio_aec_note_mic_loss, fed from
		 *       the driver's drop ledger) advance the timeline
		 *       exactly; the window jump re-anchors through the
		 *       normal drift check as a counted resync and W (kept
		 *       across resyncs) cancels again immediately;
		 *   (b) UNREPORTED losses (the driver can't size a hardware
		 *       FIFO clear) surface as a persistent lateness floor
		 *       in the capture-epoch observations; the late-floor
		 *       backstop slides the epoch by the windowed-min
		 *       lateness and the drift check re-anchors.
		 * Build with -DSC15_NO_LOSS_HANDLING -DAEC_LATE_FLOOR_MS=1000000
		 * for the pre-fix driver/AEC: BOTH checks must fail -
		 * margin runaway ~320/drop and ERLE collapse once the
		 * cumulative slip passes the window span.
		 */
		struct audio_aec_stats st0, st;
		size_t ntaps;

		/* (a) reported losses: one dropped block every 2s, 15x */
		audio_aec_enable(true);
		for (int b = 0; b < 150; b++) {
			step(4); /* 3s convergence */
		}
		audio_aec_snapshot(&st0, &ntaps);

		float worst = 1e9f;

		for (int k = 0; k < 15; k++) {
			g_drop_mic = 1;
			step(4);
			g_drop_mic = 0;
#ifndef SC15_NO_LOSS_HANDLING
			audio_aec_note_mic_loss(BLK);
#endif
			double cpi = 0, cpo = 0;

			for (int b = 0; b < 99; b++) {
				struct blkstat s = step(4);

				/* score the last 1s of the cycle: resync +
				 * history refill are long done by then
				 */
				if (b >= 49 && s.in_rms > -55.0f) {
					cpi += pow(10, s.in_rms / 10);
					cpo += pow(10, s.out_rms / 10);
				}
			}
			float e = (float)(10 * log10(cpi / (cpo + 1e-30)));

			if (e < worst) {
				worst = e;
			}
		}
		audio_aec_snapshot(&st, &ntaps);
		snprintf(buf, sizeof(buf),
			 "worst cycle %.1f dB, resyncs %u, mic_lost %llu, "
			 "margin_max %d (baseline %d), margin_last %d",
			 worst, st.resyncs,
			 (unsigned long long)st.mic_lost, st.margin_max,
			 st0.margin_max, st.margin_last);
#ifdef CONFIG_HALO_AUDIO_AEC_FDAF
		check("reported capture losses: cancellation holds",
		      worst > 8.0f && st.margin_max < st0.margin_max + 1000,
		      buf);
#else
		check("reported capture losses: cancellation holds",
		      worst > 1.5f && st.margin_max < st0.margin_max + 1000,
		      buf);
#endif

		/* (b) unreported losses: 5-drop burst, late-floor recovery */
		audio_aec_enable(true);
		for (int b = 0; b < 150; b++) {
			step(4);
		}
		audio_aec_snapshot(&st0, &ntaps);
		for (int k = 0; k < 5; k++) {
			g_drop_mic = 1;
			step(4);
			g_drop_mic = 0;
			for (int b = 0; b < 19; b++) {
				step(4);
			}
		}

		double pi = 0, po = 0;

		for (int b = 0; b < 1000; b++) { /* 20s */
			struct blkstat s = step(4);

			if (b >= 750 && s.in_rms > -55.0f) { /* final 5s */
				pi += pow(10, s.in_rms / 10);
				po += pow(10, s.out_rms / 10);
			}
		}
		audio_aec_snapshot(&st, &ntaps);

		float erle = (float)(10 * log10(pi / (po + 1e-30)));

		snprintf(buf, sizeof(buf),
			 "final-5s ERLE %.1f dB, cap_slips %u (+%ums), "
			 "resyncs %u, margin_max %d (baseline %d), "
			 "margin_last %d",
			 erle, st.cap_slips, st.cap_slip_ms, st.resyncs,
			 st.margin_max, st0.margin_max, st.margin_last);
#ifdef CONFIG_HALO_AUDIO_AEC_FDAF
		check("unreported capture losses: late-floor recovers",
		      erle > 8.0f && st.cap_slips >= 1 &&
			      st.cap_slip_ms >= 60 &&
			      st.margin_max < st0.margin_max + 2200,
		      buf);
#else
		check("unreported capture losses: late-floor recovers",
		      erle > 1.5f && st.cap_slips >= 1 &&
			      st.cap_slip_ms >= 60 &&
			      st.margin_max < st0.margin_max + 2200,
		      buf);
#endif

		/* (c) backlog flush -> too-late anchor recovery (worn Halo
		 * 08 session 212454, 2026-07-15: near-end crushed to -90dBFS
		 * for the last ~60s). When a mic backlog the backstop already
		 * reacted to finally drains in a burst (or the capture clock
		 * runs ahead over a long session), mic_total jumps and the
		 * capture-epoch observation drops persistently UNDER the
		 * frozen epoch. epoch_observe refines only earlier and only
		 * inside the (long-closed) freeze, and the forward slide only
		 * moves the epoch later, so a forward-ONLY backstop leaves the
		 * anchor stuck too late: cap_end runs ahead of real time, the
		 * reference window walks past the write head, margin latches
		 * negative and the near-end crushes to silence for the rest of
		 * the session - no cap_slip, cap_late_ms climbing unbounded,
		 * the exact 212454 signature. The symmetric (earlier) slide
		 * must pull the epoch back and re-anchor within one window.
		 * Build the forward-only backstop (-DAEC_LATE_FLOOR_FWD_ONLY)
		 * to see this FAIL: cap_slips stays 0, cap_late_ms climbs
		 * without bound (measured here 80k+ms and rising, vs ~7.5k
		 * frozen with the fix) and the margin only limps back over ~8s
		 * on the servo alone while the reference stays unreliable.
		 */
		audio_aec_enable(true);
		for (int b = 0; b < 250; b++) {
			step(4); /* 5s: converge, then run well past the 2s
				  * epoch freeze so it is firmly closed (no
				  * refine-down can absorb the flush below) */
		}
		audio_aec_snapshot(&st0, &ntaps);

		/* the drain: ~100ms of held mic samples arrive in a burst
		 * (mic_total jumps, wall clock frozen), dropping the epoch
		 * observation ~100ms below the frozen anchor - obs can only
		 * be pulled back earlier by the symmetric backstop */
		flush_mic(5);

		double qi = 0, qo = 0;
		int32_t worst_margin = 0;

		for (int b = 0; b < 1000; b++) { /* 20s recovery */
			struct blkstat s = step(4);
			struct audio_aec_stats sm;

			audio_aec_snapshot(&sm, &ntaps);
			if (sm.margin_last < worst_margin) {
				worst_margin = sm.margin_last;
			}
			if (b >= 750 && s.in_rms > -55.0f) { /* final 5s */
				qi += pow(10, s.in_rms / 10);
				qo += pow(10, s.out_rms / 10);
			}
		}
		audio_aec_snapshot(&st, &ntaps);

		float erle_c = (float)(10 * log10(qi / (qo + 1e-30)));

		snprintf(buf, sizeof(buf),
			 "final-5s ERLE %.1f dB, cap_slips %u->%u, worst "
			 "margin %d, margin_last %d, cap_late_ms %u, resyncs %u",
			 erle_c, st0.cap_slips, st.cap_slips, worst_margin,
			 st.margin_last, st.cap_late_ms, st.resyncs);
		/* Build-independent discriminator (ERLE is not usable here: the
		 * FDAF suppressor ducks so hard on the latched REF_UNREL that
		 * the pre-fix build can post a HIGHER ERLE while it crushes the
		 * near-end). worst_margin < -400: the walk-off really occurred,
		 * so the test bites. cap_slips increment: the backward slide
		 * fired. cap_late_ms bounded: the epoch re-anchored so obs stops
		 * running below it - forward-only leaves it climbing unbounded
		 * (80k+ here). margin_last >= floor-deadband: the resync
		 * re-anchored to the POSITIVE margin floor (AEC_MARGIN_RESYNC_
		 * FLOOR), not the near-zero/negative anchor that latches the
		 * REF_UNREL blanket duck - pre-clamp this landed at +16. */
		check("backlog flush: too-late anchor recovers",
		      worst_margin < -400 && st.cap_slips > st0.cap_slips &&
			      st.cap_late_ms < 20000 && st.margin_last >= 40,
		      buf);
	}

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "OK",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
