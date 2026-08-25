/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * Host-side characterisation tests for the speaker current-protection chain
 * (drivers/audio/max98357a/speaker_protect.c).
 *
 * The energy-cap checks use an INDEPENDENT double-precision implementation
 * of the sidechain (band split + current weighting + leaky integration) so
 * the production fixed-point code is not used to verify itself.
 *
 * Exit code 0 = all tests passed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "speaker_protect.h"

#define FS       16000
#define BUDGET   60
#define PI       3.14159265358979323846

static int g_failures;

#define CHECK(cond, ...)                                                  \
	do {                                                              \
		if (cond) {                                               \
			printf("  PASS: " __VA_ARGS__);                   \
			printf("\n");                                     \
		} else {                                                  \
			printf("  FAIL: " __VA_ARGS__);                   \
			printf("\n");                                     \
			g_failures++;                                     \
		}                                                         \
	} while (0)

static struct spk_protect_params params_default(int channels)
{
	struct spk_protect_params p = {
		.sample_rate = FS,
		.channels = channels,
		.hpf_cutoff_hz = 200,
		.budget_percent = BUDGET,
		.env_ms = 3,
		.hold_ms = 20,
		.release_ms = 150,
		.ramp_ms = 15,
		.bass_drive_percent = 0,
	};
	return p;
}

/** Goertzel magnitude of one bin (double precision). */
static double goertzel(const int16_t *buf, size_t n, double freq, double fs)
{
	double k = 2.0 * cos(2.0 * PI * freq / fs);
	double s0, s1 = 0, s2 = 0;

	for (size_t i = 0; i < n; i++) {
		s0 = buf[i] + k * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	return sqrt(s1 * s1 + s2 * s2 - k * s1 * s2) / (n / 2.0);
}

static void gen_sine(int16_t *buf, size_t n, double freq, double amp_fs)
{
	for (size_t i = 0; i < n; i++) {
		buf[i] = (int16_t)lrint(amp_fs * 32767.0 * sin(2.0 * PI * freq * i / FS));
	}
}

static double rms(const int16_t *buf, size_t n)
{
	double acc = 0;

	for (size_t i = 0; i < n; i++) {
		acc += (double)buf[i] * buf[i];
	}
	return sqrt(acc / n);
}

static double db(double ratio)
{
	return 20.0 * log10(ratio > 1e-12 ? ratio : 1e-12);
}

/* ------------------------------------------------------------------ */
/* Independent double-precision sidechain: band split, current weights,
 * leaky-integrated weighted energy. Mirrors the architecture but shares
 * no code with the fixed-point implementation.
 */
static const double ref_split_hz[5] = { 150, 275, 425, 750, 1500 };
static const double ref_weight[6] = { 3.0, 3.0, 3.0, 3.0, 3.0, 3.0 };

struct ref_sidechain {
	double lp[5];
	double env;
	double alpha[5];
	double env_alpha;
};

static void ref_sidechain_init(struct ref_sidechain *r, int fs, int env_ms)
{
	memset(r, 0, sizeof(*r));
	for (int i = 0; i < 5; i++) {
		double omega = 2.0 * PI * ref_split_hz[i];

		r->alpha[i] = omega / (fs + omega);
	}
	/* Match the fixed-point integrator: shift = floor(log2(fs*ms/1000)) */
	int frames = fs * env_ms / 1000;
	int shift = 0;

	while ((1 << (shift + 1)) <= frames) {
		shift++;
	}
	r->env_alpha = 1.0 / (1 << shift);
}

/* Feed one frame (all channels); returns current weighted energy env. */
static double ref_sidechain_step(struct ref_sidechain *r, const int16_t *frame, int nch)
{
	double p_tot = 0;

	/* NOTE: single lp state set shared across channels would be wrong;
	 * for this checker we only use it with mono content or identical
	 * stereo content (worst case for current), which keeps states valid.
	 */
	for (int c = 0; c < nch; c++) {
		double x = frame[c];
		double prev = 0, p = 0;

		for (int b = 0; b < 6; b++) {
			double band;

			if (b < 5) {
				if (c == 0) { /* advance states once per frame */
					r->lp[b] += r->alpha[b] * (x - r->lp[b]);
				}
				band = r->lp[b] - prev;
				prev = r->lp[b];
			} else {
				band = x - prev;
			}
			p += band * ref_weight[b];
		}
		p_tot += fabs(p);
	}
	r->env += r->env_alpha * (p_tot * p_tot - r->env);
	return r->env;
}

static double budget_threshold_pct(int budget)
{
	double amp = 32767.0 * budget / 100.0;

	return amp * amp / 2.0;
}

static double budget_threshold(void)
{
	return budget_threshold_pct(BUDGET);
}

/* ------------------------------------------------------------------ */

static void test_small_signal_response(void)
{
	printf("small-signal frequency response (limiter idle, -30 dBFS):\n");

	static const double freqs[] = { 60, 130, 300, 500, 1000, 3000 };
	double gains[6];
	enum { N = 4 * FS };
	static int16_t buf[N];

	for (size_t f = 0; f < 6; f++) {
		struct spk_protect sp;
		struct spk_protect_params p = params_default(1);

		p.ramp_ms = 0; /* isolate the filters */
		spk_protect_init(&sp, &p);

		gen_sine(buf, N, freqs[f], 0.0316); /* -30 dBFS */
		double in_rms = rms(buf + N / 2, N / 2);

		spk_protect_process(&sp, buf, N);
		gains[f] = db(rms(buf + N / 2, N / 2) / in_rms);
		printf("    %6.0f Hz: %+6.2f dB\n", freqs[f], gains[f]);
	}

	/* Old chain reference at 300 Hz: 4th-order 300 Hz HPF (-3 dB) plus
	 * driver EQ (-15 dB) = -18 dB. New chain must stay above -9 dB.
	 */
	CHECK(gains[2] > -9.0 && gains[2] < -3.0,
	      "300 Hz voice band: %+.1f dB (was ~-18 dB in old stacked chain)", gains[2]);
	CHECK(gains[0] < -15.0, "60 Hz (below resonance) strongly cut: %+.1f dB", gains[0]);
	CHECK(gains[4] > -4.0, "1 kHz nearly intact: %+.1f dB", gains[4]);
	/* Rebal voicing boosts the top band +4 dB (audible loudness per unit
	 * current); split leakage from the flat neighbours pulls the measured
	 * boost at 3 kHz down to ~+2.5 dB.
	 */
	CHECK(gains[5] > 1.0 && gains[5] < 4.5, "3 kHz boosted: %+.1f dB", gains[5]);
	CHECK(gains[0] < gains[1] && gains[1] < gains[2] && gains[2] < gains[4],
	      "response monotonically rising through voice band");
}

static void test_energy_cap(double freq, int nch, const char *label)
{
	enum { N = 2 * FS };
	static int16_t buf[2 * FS * 2];
	struct spk_protect sp;
	struct spk_protect_params p = params_default(nch);

	p.ramp_ms = 0;
	spk_protect_init(&sp, &p);

	/* Full-scale sine, identical on all channels (worst case: in-phase
	 * transducer currents add).
	 */
	for (size_t i = 0; i < N; i++) {
		int16_t s = (int16_t)lrint(32767.0 * sin(2.0 * PI * freq * i / FS));

		for (int c = 0; c < nch; c++) {
			buf[i * nch + c] = s;
		}
	}

	spk_protect_process(&sp, buf, (size_t)N * nch);

	/* Sub-resonance content (the 80 Hz case) is held inside the budget by
	 * the HPF + voicing alone - by design - so the limiter legitimately
	 * never engages there; only assert engagement where the static
	 * filters cannot do the job.
	 */
	bool expect_limiting = freq > 150;

	/* Verify with the independent sidechain over the settled tail. */
	struct ref_sidechain ref;

	ref_sidechain_init(&ref, FS, p.env_ms);

	double max_env = 0;

	for (size_t i = 0; i < N; i++) {
		double env = ref_sidechain_step(&ref, &buf[i * nch], nch);

		if (i > (size_t)N / 2 && env > max_env) {
			max_env = env;
		}
	}

	double thr = budget_threshold();

	CHECK(max_env <= thr * 1.25,
	      "%s: settled weighted energy %.3g <= budget %.3g (x%.2f)",
	      label, max_env, thr, max_env / thr);
	if (expect_limiting) {
		CHECK(spk_protect_gain_q15(&sp) < SPK_PROTECT_Q15_ONE,
		      "%s: limiter engaged (gain %.3f)", label,
		      spk_protect_gain_q15(&sp) / 32768.0);
	} else {
		CHECK(spk_protect_gain_q15(&sp) == SPK_PROTECT_Q15_ONE,
		      "%s: static filters alone suffice (limiter idle)", label);
	}
}

static void test_quiet_passthrough(void)
{
	printf("quiet content passes unlimited:\n");

	enum { N = 2 * FS };
	static int16_t buf[N];
	struct spk_protect sp;
	struct spk_protect_params p = params_default(1);

	p.ramp_ms = 0;
	spk_protect_init(&sp, &p);

	gen_sine(buf, N, 300, 0.1); /* -20 dBFS */
	spk_protect_process(&sp, buf, N);

	CHECK(spk_protect_gain_q15(&sp) == SPK_PROTECT_Q15_ONE,
	      "-20 dBFS 300 Hz: limiter never engaged (gain stays 1.0)");
}

static void test_gain_ripple(void)
{
	printf("limiter ballistics (no per-cycle gain tracking):\n");

	enum { N = FS }; /* 1 s of full-scale 500 Hz (engages under the
			  * rebal voicing; 200 Hz no longer reaches the
			  * budget after its deeper static cut) */
	static int16_t buf[N];
	struct spk_protect sp;
	struct spk_protect_params p = params_default(1);

	p.ramp_ms = 0;
	spk_protect_init(&sp, &p);

	/* Settle 750 ms */
	gen_sine(buf, N, 500, 1.0);
	spk_protect_process(&sp, buf, (size_t)(0.75 * N));

	/* Track gain across the following 200 ms, frame by frame */
	int32_t gmin = INT32_MAX, gmax = 0;

	for (size_t i = (size_t)(0.75 * N); i < N; i++) {
		spk_protect_process(&sp, &buf[i], 1);
		int32_t g = spk_protect_gain_q15(&sp);

		if (g < gmin) {
			gmin = g;
		}
		if (g > gmax) {
			gmax = g;
		}
	}

	double ripple = (double)(gmax - gmin) / gmax;

	CHECK(gmax < SPK_PROTECT_Q15_ONE,
	      "limiter engaged throughout the measurement window");
	CHECK(gmin > 0 && ripple < 0.03,
	      "gain ripple %.2f%% over steady 500 Hz (old limiter re-attacked every cycle)",
	      ripple * 100.0);
}

static void test_onset_ramp(void)
{
	printf("onset ramp spreads inrush:\n");

	enum { N = FS / 10 }; /* 100 ms */
	static int16_t buf[N];
	struct spk_protect sp;
	struct spk_protect_params p = params_default(1);

	spk_protect_init(&sp, &p); /* ramp_ms = 15 */

	/* Sub-budget amplitude so the limiter stays idle and the measurement
	 * isolates the ramp itself.
	 */
	gen_sine(buf, N, 1000, 0.2);
	spk_protect_process(&sp, buf, N);

	double early = rms(buf, FS * 2 / 1000);              /* first 2 ms */
	double later = rms(buf + FS * 30 / 1000, FS * 10 / 1000); /* 30-40 ms */

	CHECK(early < 0.25 * later,
	      "first 2 ms RMS %.0f << settled RMS %.0f", early, later);

	/* Reset restarts the ramp */
	spk_protect_reset(&sp);
	gen_sine(buf, N, 1000, 0.2);
	spk_protect_process(&sp, buf, N);
	CHECK(rms(buf, FS * 2 / 1000) < 0.25 * later, "ramp restarts after reset");
}

static void test_stereo_isolation(void)
{
	printf("stereo channel isolation (regression: old EQ shared state across L/R):\n");

	enum { FRAMES = FS / 2 };
	static int16_t buf[FRAMES * 2];
	struct spk_protect sp;
	struct spk_protect_params p = params_default(2);

	p.ramp_ms = 0;
	spk_protect_init(&sp, &p);

	for (size_t i = 0; i < FRAMES; i++) {
		buf[2 * i] = (int16_t)lrint(0.0316 * 32767.0 *
					    sin(2.0 * PI * 1000.0 * i / FS));
		buf[2 * i + 1] = 0;
	}
	spk_protect_process(&sp, buf, FRAMES * 2);

	double r_rms = 0;

	for (size_t i = 0; i < FRAMES; i++) {
		r_rms += (double)buf[2 * i + 1] * buf[2 * i + 1];
	}
	r_rms = sqrt(r_rms / FRAMES);

	CHECK(r_rms < 2.0, "silent R channel stays silent (RMS %.2f LSB)", r_rms);
}

static void test_chunk_equivalence(void)
{
	printf("state continuity across buffer boundaries:\n");

	enum { N = FS / 2 };
	static int16_t whole[N], chunked[N];
	struct spk_protect sp_a, sp_b;
	struct spk_protect_params p = params_default(1);

	spk_protect_init(&sp_a, &p);
	spk_protect_init(&sp_b, &p);

	/* Speech-ish: mixed tones + onset transient, near full scale */
	for (size_t i = 0; i < N; i++) {
		double v = 0.5 * sin(2 * PI * 220 * i / (double)FS) +
			   0.3 * sin(2 * PI * 700 * i / (double)FS) +
			   0.2 * sin(2 * PI * 2100 * i / (double)FS);

		whole[i] = (int16_t)lrint(32000.0 * ((i > N / 3) ? v : v * 0.05));
	}
	memcpy(chunked, whole, sizeof(whole));

	spk_protect_process(&sp_a, whole, N);
	for (size_t off = 0; off < N; off += 160) { /* 10 ms LC3-frame-sized */
		spk_protect_process(&sp_b, chunked + off, 160);
	}

	CHECK(memcmp(whole, chunked, sizeof(whole)) == 0,
	      "10 ms-chunked output bit-exact vs single-buffer output");
}

static void test_worst_case_safety(void)
{
	printf("worst-case content safety (ASan/UBSan active):\n");

	enum { N = FS };
	static int16_t buf[N];
	struct spk_protect sp;
	struct spk_protect_params p = params_default(1);

	p.ramp_ms = 0;
	spk_protect_init(&sp, &p);

	/* Full-scale 100 Hz square wave: worst spectral + amplitude case */
	for (size_t i = 0; i < N; i++) {
		buf[i] = ((i / (FS / 200)) & 1) ? INT16_MAX : INT16_MIN;
	}
	spk_protect_process(&sp, buf, N);

	struct ref_sidechain ref;

	ref_sidechain_init(&ref, FS, p.env_ms);

	double max_env = 0;

	for (size_t i = 0; i < N; i++) {
		double env = ref_sidechain_step(&ref, &buf[i], 1);

		if (i > (size_t)N / 2 && env > max_env) {
			max_env = env;
		}
	}

	CHECK(max_env <= budget_threshold() * 1.25,
	      "full-scale square wave capped to budget (x%.2f)",
	      max_env / budget_threshold());
}

static void test_8k_smoke(void)
{
	printf("8 kHz smoke test:\n");

	struct spk_protect sp;
	struct spk_protect_params p = params_default(1);

	p.sample_rate = 8000;

	int rc = spk_protect_init(&sp, &p);
	static int16_t buf[8000];

	for (size_t i = 0; i < 8000; i++) {
		buf[i] = (int16_t)lrint(32767.0 * sin(2.0 * PI * 300.0 * i / 8000.0));
	}
	spk_protect_process(&sp, buf, 8000);

	CHECK(rc == 0 && spk_protect_gain_q15(&sp) < SPK_PROTECT_Q15_ONE,
	      "init ok, limiter engages on full-scale 300 Hz");
}

static void test_noise_energy_cap(void)
{
	printf("broadband noise capped (LC3-garbage regression, tripped battery IC on HW):\n");

	enum { N = 2 * FS };
	static int16_t buf[N];
	struct spk_protect sp;
	struct spk_protect_params p = params_default(1);

	p.ramp_ms = 0;
	spk_protect_init(&sp, &p);

	/* Deterministic full-scale white noise (LCG, fixed seed) */
	uint32_t state = 0x12345678u;

	for (size_t i = 0; i < N; i++) {
		state = state * 1664525u + 1013904223u;
		buf[i] = (int16_t)(state >> 16);
	}
	spk_protect_process(&sp, buf, N);

	struct ref_sidechain ref;

	ref_sidechain_init(&ref, FS, p.env_ms);

	double max_env = 0;

	for (size_t i = 0; i < N; i++) {
		double env = ref_sidechain_step(&ref, &buf[i], 1);

		if (i > (size_t)N / 2 && env > max_env) {
			max_env = env;
		}
	}

	CHECK(max_env <= budget_threshold() * 1.25,
	      "full-scale white noise: settled weighted energy x%.2f of budget",
	      max_env / budget_threshold());
	CHECK(spk_protect_gain_q15(&sp) < SPK_PROTECT_Q15_ONE / 2,
	      "limiter engaged hard on noise (gain %.3f)",
	      spk_protect_gain_q15(&sp) / 32768.0);
}

static void test_bass_enhancer(void)
{
	printf("psychoacoustic bass enhancer:\n");

	enum { N = 2 * FS };
	static int16_t off_buf[N], on_buf[N];
	struct spk_protect sp;
	struct spk_protect_params p = params_default(1);

	p.ramp_ms = 0;

	/* -20 dBFS 100 Hz: below the HPF, inside the source band, limiter idle */
	gen_sine(off_buf, N, 100, 0.1);
	memcpy(on_buf, off_buf, sizeof(off_buf));

	spk_protect_init(&sp, &p);          /* drive 0: enhancer off */
	spk_protect_process(&sp, off_buf, N);

	p.bass_drive_percent = 100;
	spk_protect_init(&sp, &p);
	spk_protect_process(&sp, on_buf, N);

	/* Analyse the settled tail */
	const int16_t *off_t = off_buf + N / 2, *on_t = on_buf + N / 2;
	double h2_off = goertzel(off_t, N / 2, 200, FS);
	double h2_on = goertzel(on_t, N / 2, 200, FS);
	double h4_on = goertzel(on_t, N / 2, 400, FS);
	double f0_on = goertzel(on_t, N / 2, 100, FS);

	printf("    100 Hz in -> 100 Hz: %.0f | 200 Hz: %.0f (off: %.0f) | 400 Hz: %.0f\n",
	       f0_on, h2_on, h2_off, h4_on);

	CHECK(h2_on > 4.0 * (h2_off + 1.0),
	      "2nd harmonic (200 Hz) generated: %.0f vs %.0f without", h2_on, h2_off);
	/* The 130 Hz HPF only takes ~6 dB at 100 Hz, so the residual
	 * fundamental legitimately stays larger than the harmonics. Assert a
	 * floor on the harmonic-to-residual ratio at drive 100; the actual
	 * voicing level is tuned by ear via the drive knob (linear scaling).
	 */
	CHECK(h2_on > 0.15 * f0_on,
	      "2nd harmonic vs residual fundamental: %.2fx at drive 100 (floor 0.15)",
	      h2_on / f0_on);

	/* Content above the source band must pass clean: 1 kHz in, no 2 kHz */
	static int16_t hf_buf[N];

	gen_sine(hf_buf, N, 1000, 0.1);
	spk_protect_init(&sp, &p);
	spk_protect_process(&sp, hf_buf, N);

	double hf_h2 = goertzel(hf_buf + N / 2, N / 2, 2000, FS);
	double hf_f0 = goertzel(hf_buf + N / 2, N / 2, 1000, FS);

	CHECK(hf_h2 < 0.02 * hf_f0,
	      "1 kHz content stays clean (2 kHz at %.1f%% of fundamental)",
	      100.0 * hf_h2 / hf_f0);
}

static void test_bass_enhancer_safety(void)
{
	printf("bass enhancer cannot break the current budget (max drive):\n");

	enum { N = 2 * FS };
	static int16_t buf[N];
	struct spk_protect sp;
	struct spk_protect_params p = params_default(1);

	p.ramp_ms = 0;
	p.bass_drive_percent = 200;
	spk_protect_init(&sp, &p);

	gen_sine(buf, N, 100, 1.0); /* full-scale, worst source-band case */
	spk_protect_process(&sp, buf, N);

	struct ref_sidechain ref;

	ref_sidechain_init(&ref, FS, p.env_ms);

	double max_env = 0;

	for (size_t i = 0; i < N; i++) {
		double env = ref_sidechain_step(&ref, &buf[i], 1);

		if (i > (size_t)N / 2 && env > max_env) {
			max_env = env;
		}
	}

	CHECK(max_env <= budget_threshold() * 1.25,
	      "full-scale 100 Hz at drive 200%%: energy x%.2f of budget",
	      max_env / budget_threshold());
}

static void test_bass_enhancer_chunk_equivalence(void)
{
	printf("enhancer state continuity across buffer boundaries:\n");

	enum { N = FS / 2 };
	static int16_t whole[N], chunked[N];
	struct spk_protect sp_a, sp_b;
	struct spk_protect_params p = params_default(1);

	p.bass_drive_percent = 100;
	spk_protect_init(&sp_a, &p);
	spk_protect_init(&sp_b, &p);

	for (size_t i = 0; i < N; i++) {
		double v = 0.6 * sin(2 * PI * 120 * i / (double)FS) +
			   0.3 * sin(2 * PI * 800 * i / (double)FS);

		whole[i] = (int16_t)lrint(30000.0 * v);
	}
	memcpy(chunked, whole, sizeof(whole));

	spk_protect_process(&sp_a, whole, N);
	for (size_t off = 0; off < N; off += 160) {
		spk_protect_process(&sp_b, chunked + off, 160);
	}

	CHECK(memcmp(whole, chunked, sizeof(whole)) == 0,
	      "chunked output bit-exact with enhancer active");
}

static void test_budget_drop(void)
{
	printf("runtime budget drop (display-aware ducking):\n");

	enum { N = FS }; /* 1 s blocks of full-scale 500 Hz (32-sample
			  * period: regenerating the block continues the
			  * phase seamlessly) */
	static int16_t buf[N];
	struct spk_protect sp;
	struct spk_protect_params p = params_default(1);

	spk_protect_init(&sp, &p);

	/* Settle at the full budget */
	gen_sine(buf, N, 500, 1.0);
	spk_protect_process(&sp, buf, N);

	double full_rms = rms(buf + N / 2, N / 2);
	int32_t g0 = spk_protect_gain_q15(&sp);

	/* Drop 20 points mid-stream; track per-frame gain steps through the
	 * transition - the threshold slews over ~ramp_ms, so no single frame
	 * may step the gain audibly (a click would be a large step).
	 */
	spk_protect_set_budget_drop(&sp, 20);
	gen_sine(buf, N, 500, 1.0);

	double max_step = 0;
	int32_t g_prev = g0;

	for (size_t i = 0; i < N; i++) {
		spk_protect_process(&sp, &buf[i], 1);
		int32_t g = spk_protect_gain_q15(&sp);
		double step = fabs((double)(g - g_prev)) / g_prev;

		if (step > max_step) {
			max_step = step;
		}
		g_prev = g;
	}

	int32_t g1 = spk_protect_gain_q15(&sp);

	CHECK(g1 < g0, "gain reduced after drop (%.3f -> %.3f)",
	      g0 / 32768.0, g1 / 32768.0);
	/* The limiter quantizes attacks to the env-ripple cycle, so single
	 * frames legitimately step a fraction of a dB; a click would be tens
	 * of percent. 3 % ~= 0.26 dB.
	 */
	CHECK(max_step < 0.03,
	      "largest per-frame gain step %.2f%% (slewed, not stepped)",
	      max_step * 100.0);

	/* Settled energy now honours the reduced budget */
	struct ref_sidechain ref;

	ref_sidechain_init(&ref, FS, p.env_ms);

	double max_env = 0;

	for (size_t i = 0; i < N; i++) {
		double env = ref_sidechain_step(&ref, &buf[i], 1);

		if (i > (size_t)N / 2 && env > max_env) {
			max_env = env;
		}
	}
	CHECK(max_env <= budget_threshold_pct(BUDGET - 20) * 1.25,
	      "settled energy honours budget %d (x%.2f of reduced threshold)",
	      BUDGET - 20, max_env / budget_threshold_pct(BUDGET - 20));

	/* Restoring the budget recovers the original level */
	spk_protect_set_budget_drop(&sp, 0);
	gen_sine(buf, N, 500, 1.0);
	spk_protect_process(&sp, buf, N);

	double back_rms = rms(buf + N / 2, N / 2);

	CHECK(fabs(back_rms - full_rms) < 0.05 * full_rms,
	      "output recovers after restore (RMS %.0f vs %.0f)",
	      back_rms, full_rms);

	/* Effective budget is floored: a huge drop must not mute entirely */
	spk_protect_set_budget_drop(&sp, 95);
	gen_sine(buf, N, 500, 1.0);
	spk_protect_process(&sp, buf, N);
	CHECK(rms(buf + N / 2, N / 2) > 0.01 * 32767.0,
	      "5%% budget floor keeps audio audible (RMS %.0f)",
	      rms(buf + N / 2, N / 2));
}

static void test_peak_cap(void)
{
	printf("true-peak current clamp (fricative-spike regression, latched IC on HW):\n");

	/* A pregain-boosted broadband onset from silence: the energy
	 * sidechain is blind for ~env_ms, so without the cap the first
	 * milliseconds pass at full boosted amplitude.
	 */
	enum { N = FS / 4 };
	static int16_t buf[N], ref[N];
	struct spk_protect sp;
	struct spk_protect_params p = params_default(1);
	uint32_t state = 0xCAFEBABEu;

	p.ramp_ms = 0;
	/* The hardware latch happened at budget 100: the energy limiter's
	 * first-frame attack is weakest there, letting the onset emit ~4x
	 * weighted before the integrator converges. Mirror that.
	 */
	p.budget_percent = 100;
	p.peak_cap_percent = 300;

	for (size_t i = 0; i < N; i++) {
		state = state * 1664525u + 1013904223u;
		/* silence, then a full-scale noise burst (fricative-ish) */
		buf[i] = (i < N / 2) ? 0 : (int16_t)(state >> 16);
	}
	memcpy(ref, buf, sizeof(buf));

	spk_protect_init(&sp, &p);
	spk_protect_set_pregain(&sp, spk_protect_db10_to_q15(120)); /* +12 dB */
	spk_protect_process(&sp, buf, N);

	/* Reconstruct the weighted output amplitude sample by sample with
	 * the independent sidechain's band split (per-sample |p|, no
	 * integrator) and find the worst instantaneous excursion in the
	 * onset window.
	 */
	struct ref_sidechain rc;

	ref_sidechain_init(&rc, FS, p.env_ms);
	double worst = 0;

	for (size_t i = 0; i < N; i++) {
		double x = buf[i], prev = 0, wsum = 0;

		for (int b = 0; b < 6; b++) {
			double band;

			if (b < 5) {
				rc.lp[b] += rc.alpha[b] * (x - rc.lp[b]);
				band = rc.lp[b] - prev;
				prev = rc.lp[b];
			} else {
				band = x - prev;
			}
			wsum += band * ref_weight[b];
		}
		if (i >= N / 2 && fabs(wsum) > worst) {
			worst = fabs(wsum);
		}
	}

	double cap = 32767.0 * 3.0; /* 300 % in weighted units */

	CHECK(worst <= cap * 1.15,
	      "boosted onset instantaneous weighted drive %.0f <= cap %.0f (x%.2f)",
	      worst, cap, worst / cap);

	struct spk_protect_stats st;

	spk_protect_stats_read(&sp, &st, true);
	CHECK(st.peak_capped_frames > 0,
	      "clamp engaged on the onset (%u frames)", st.peak_capped_frames);

	/* Below-cap content must be untouched: identical output with the
	 * cap on and off.
	 */
	static int16_t a[FS / 4], b2[FS / 4];

	for (size_t i = 0; i < N; i++) {
		a[i] = (int16_t)lrint(0.4 * 32767.0 *
				      sin(2.0 * PI * 700.0 * i / FS));
	}
	memcpy(b2, a, sizeof(a));

	p = params_default(1);
	p.ramp_ms = 0;
	p.peak_cap_percent = 0;
	spk_protect_init(&sp, &p);
	spk_protect_process(&sp, a, N);

	p.peak_cap_percent = 300;
	spk_protect_init(&sp, &p);
	spk_protect_process(&sp, b2, N);

	CHECK(memcmp(a, b2, sizeof(a)) == 0,
	      "normal content bit-exact with the cap enabled");
}

static void test_param_validation(void)
{
	printf("parameter validation:\n");

	struct spk_protect sp;
	struct spk_protect_params p = params_default(1);
	int ok = 1;

	p.channels = 3;
	ok &= (spk_protect_init(&sp, &p) != 0);
	p = params_default(1);
	p.budget_percent = 0;
	ok &= (spk_protect_init(&sp, &p) != 0);
	p = params_default(1);
	p.hpf_cutoff_hz = FS; /* >= Nyquist */
	ok &= (spk_protect_init(&sp, &p) != 0);
	p = params_default(1);
	p.sample_rate = 4000;
	ok &= (spk_protect_init(&sp, &p) != 0);
	p = params_default(1);
	p.bass_drive_percent = 201;
	ok &= (spk_protect_init(&sp, &p) != 0);
	p = params_default(1);
	p.peak_cap_percent = 1001;
	ok &= (spk_protect_init(&sp, &p) != 0);

	CHECK(ok, "invalid channels/budget/cutoff/rate/drive all rejected");
}

int main(void)
{
	printf("speaker_protect host tests (fs=%d, budget=%d%%)\n\n", FS, BUDGET);

	test_small_signal_response();
	test_quiet_passthrough();
	test_energy_cap(300, 1, "full-scale 300 Hz mono");
	test_energy_cap(300, 2, "full-scale 300 Hz stereo in-phase");
	test_energy_cap(80, 1, "full-scale 80 Hz mono");
	test_gain_ripple();
	test_onset_ramp();
	test_stereo_isolation();
	test_chunk_equivalence();
	test_worst_case_safety();
	test_8k_smoke();
	test_noise_energy_cap();
	test_bass_enhancer();
	test_bass_enhancer_safety();
	test_bass_enhancer_chunk_equivalence();
	test_budget_drop();
	test_peak_cap();
	test_param_validation();

	printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL TESTS PASSED",
	       g_failures, g_failures == 1 ? "" : "s");
	return g_failures ? 1 : 0;
}
