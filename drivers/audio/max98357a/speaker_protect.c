/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file speaker_protect.c
 * @brief Bone-conduction speaker current-protection DSP chain.
 *
 * See speaker_protect.h for the architecture description. This file is
 * deliberately free of Zephyr dependencies so the exact production code can
 * be compiled and characterised on a host machine
 * (tests/audio/speaker_protect).
 */

#include "speaker_protect.h"

#include <string.h>
#include <math.h>

/* M_PI is a POSIX/GNU extension and not exposed by <math.h> under strict
 * C99, which Zephyr uses.
 */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define Q30_SHIFT 30
#define Q15_ONE   SPK_PROTECT_Q15_ONE

/* Band split points. Chosen around the transducer's high-current region;
 * same grid as the issue #180 characterisation so bench data maps directly.
 */
static const uint16_t split_hz[SPK_PROTECT_SPLITS] = {
	150, 275, 425, 750, 1500,
};

/* Static voicing gains per band (Q15).
 *
 * Roughly half (in dB) of the issue #180 protection curve: that curve was
 * tuned as the *only* protection; the energy limiter now carries the
 * protection duty, so the static cut only pre-shapes the response.
 *
 *   band       < 150  150-275  275-425  425-750  750-1500  > 1500  Hz
 *   #180 curve  -12     -14      -15      -11       -3        0    dB
 *   this table   -6      -7      -7.5     -5.5      -1.5      0    dB
 */
static const int32_t static_gain_q15[SPK_PROTECT_BANDS] = {
	16423, 14637, 13818, 17397, 27571, 32768,
};

/* Per-band current weights (Q12, 4096 = 1.0) for the limiter sidechain.
 *
 * Approximates SYSTEM supply-current draw per unit signal amplitude vs
 * frequency. The low bands are derived by inverting the issue #180
 * protection depths (the empirically measured "how much this band had to be
 * cut" is a direct proxy for "how much current this band draws"). The #180
 * data only characterised low-frequency content; on-device testing showed
 * full-scale BROADBAND noise tripping the battery IC even when the weighted
 * envelope was within budget (Class-D amplifier losses and transducer
 * behaviour above 750 Hz draw more than the inverted-EQ model predicted),
 * so the two top bands carry defensive uplifts (1.4 -> 2.5, 1.0 -> 2.0)
 * pending bench calibration that must include noise, not just sine sweeps.
 *
 * Budget reference: a >= 1.5 kHz sine at weight 2.0 - i.e. budget_percent
 * is measured against the top band's weighted amplitude.
 *
 *   band    < 150  150-275  275-425  425-750  750-1500  > 1500  Hz
 *   weight   4.0     5.0      5.6      3.5      2.5       2.0
 */
static const int32_t current_weight_q12[SPK_PROTECT_BANDS] = {
	16384, 20480, 22938, 14336, 10240, 8192,
};

/* Bands below this index get the limiter gain cubed (3x the dB reduction)
 * so gain reduction lands where the current is drawn: bands 0..2 (< 425 Hz).
 */
#define LF_DUCK_BANDS 3

/* Soft clipper: knee at 90 % FS, 1/4 slope above the knee. */
#define SOFT_CLIP_KNEE  29491
#define SOFT_CLIP_SHIFT 2

static inline int16_t sat_i16(int32_t v)
{
	if (v > INT16_MAX) {
		return INT16_MAX;
	}
	if (v < INT16_MIN) {
		return INT16_MIN;
	}
	return (int16_t)v;
}

/** floor(log2(v)), v >= 1 */
static int32_t ilog2_i32(int32_t v)
{
	int32_t r = 0;

	while (v > 1) {
		v >>= 1;
		r++;
	}
	return r;
}

/** Integer sqrt of a non-negative int64, result <= 2^31 - 1. */
static int64_t isqrt64(int64_t val)
{
	uint64_t v = (uint64_t)val;
	uint64_t rem = 0;
	uint64_t root = 0;

	for (int i = 0; i < 32; i++) {
		root <<= 1;
		rem = (rem << 2) | (v >> 62);
		v <<= 2;
		if (root < rem) {
			rem -= root + 1;
			root += 2;
		}
	}
	return (int64_t)(root >> 1);
}

int spk_protect_init(struct spk_protect *sp, const struct spk_protect_params *params)
{
	if (!sp || !params) {
		return -1;
	}
	if (params->sample_rate < 8000 || params->sample_rate > 48000) {
		return -1;
	}
	if (params->channels < 1 || params->channels > SPK_PROTECT_MAX_CHANNELS) {
		return -1;
	}
	if (params->budget_percent < 1 || params->budget_percent > 100) {
		return -1;
	}
	if (params->hpf_cutoff_hz < 0 ||
	    params->hpf_cutoff_hz >= params->sample_rate / 2) {
		return -1;
	}
	if (params->env_ms < 1 || params->hold_ms < 0 ||
	    params->release_ms < 1 || params->ramp_ms < 0) {
		return -1;
	}

	memset(sp, 0, sizeof(*sp));
	sp->params = *params;

	/* --- HPF: 2nd-order Butterworth (Q = 1/sqrt(2)), bilinear transform.
	 * Double math at init only; the hot path is pure fixed point.
	 * (Same approach as modules/halo audio_eq.c.)
	 */
	if (params->hpf_cutoff_hz > 0) {
		double w0 = 2.0 * M_PI * (double)params->hpf_cutoff_hz /
			    (double)params->sample_rate;
		double cw = cos(w0);
		double alpha = sin(w0) / (2.0 * 0.7071067811865476);
		double a0 = 1.0 + alpha;
		double scale = (double)(1 << Q30_SHIFT) / a0;

		sp->hpf_b0 = (int32_t)(((1.0 + cw) / 2.0) * scale + 0.5);
		sp->hpf_b1 = (int32_t)(-(1.0 + cw) * scale - 0.5);
		sp->hpf_b2 = sp->hpf_b0;
		sp->hpf_a1 = (int32_t)((-2.0 * cw) * scale - 0.5);
		sp->hpf_a2 = (int32_t)((1.0 - alpha) * scale + 0.5);
		sp->hpf_enabled = true;
	}

	/* --- One-pole split coefficients: alpha = 2*pi*f / (fs + 2*pi*f) --- */
	for (int i = 0; i < SPK_PROTECT_SPLITS; i++) {
		double omega = 2.0 * M_PI * (double)split_hz[i];

		sp->alpha_q15[i] = (int32_t)((double)Q15_ONE * omega /
					     ((double)params->sample_rate + omega) + 0.5);
	}

	/* --- Limiter sidechain ---
	 * Envelope integrator approximates the battery protection IC's
	 * over-current detection window: env += (p^2 - env) >> env_shift,
	 * time constant ~= 2^env_shift frames.
	 */
	sp->env_shift = ilog2_i32((int32_t)((int64_t)params->sample_rate *
					    params->env_ms / 1000));
	if (sp->env_shift < 1) {
		sp->env_shift = 1;
	}

	/* Budget: steady-state envelope of a reference-band (weight 1.0)
	 * sine of amplitude budget_percent * FS is A^2 / 2.
	 */
	{
		int64_t amp = (32767LL * params->budget_percent) / 100;

		sp->threshold_env = (amp * amp) / 2;
	}

	sp->hold_samples = (int32_t)((int64_t)params->sample_rate *
				     params->hold_ms / 1000);

	sp->release_shift = ilog2_i32((int32_t)((int64_t)params->sample_rate *
						params->release_ms / 1000));
	if (sp->release_shift < 1) {
		sp->release_shift = 1;
	}

	if (params->ramp_ms > 0) {
		int32_t ramp_frames = (int32_t)((int64_t)params->sample_rate *
						params->ramp_ms / 1000);

		sp->ramp_step = Q15_ONE / (ramp_frames > 0 ? ramp_frames : 1);
		if (sp->ramp_step < 1) {
			sp->ramp_step = 1;
		}
	} else {
		sp->ramp_step = Q15_ONE;
	}

	spk_protect_reset(sp);
	return 0;
}

void spk_protect_reset(struct spk_protect *sp)
{
	if (!sp) {
		return;
	}

	memset(sp->ch, 0, sizeof(sp->ch));
	sp->env = 0;
	sp->gain_q15 = Q15_ONE;
	sp->hold_left = 0;
	sp->ramp_q15 = (sp->params.ramp_ms > 0) ? 0 : Q15_ONE;
}

/** HPF biquad, Direct Form II Transposed, Q30 coefficients. */
static inline int32_t hpf_process(struct spk_protect *sp,
				  struct spk_protect_channel *ch, int32_t x)
{
	int64_t y = ((int64_t)sp->hpf_b0 * x + ch->hpf_z1) >> Q30_SHIFT;

	ch->hpf_z1 = (int64_t)sp->hpf_b1 * x - (int64_t)sp->hpf_a1 * y + ch->hpf_z2;
	ch->hpf_z2 = (int64_t)sp->hpf_b2 * x - (int64_t)sp->hpf_a2 * y;

	return (int32_t)y;
}

void spk_protect_process(struct spk_protect *sp, int16_t *pcm, size_t num_samples)
{
	if (!sp || !pcm || num_samples == 0) {
		return;
	}

	const int nch = sp->params.channels;
	/* Per-channel scratch, Q15-extended sample domain */
	int64_t y_lf[SPK_PROTECT_MAX_CHANNELS];
	int64_t y_hf[SPK_PROTECT_MAX_CHANNELS];

	/* Frame-based: a trailing partial frame (only possible if a caller
	 * writes buffers not aligned to whole frames) is left untouched.
	 * The driver's DMA chunks are always frame-aligned.
	 */
	for (size_t i = 0; i + nch <= num_samples; i += nch) {
		int64_t frame_proxy = 0;

		for (int c = 0; c < nch; c++) {
			struct spk_protect_channel *ch = &sp->ch[c];
			int32_t x = pcm[i + c];

			/* Onset ramp */
			x = (int32_t)(((int64_t)x * sp->ramp_q15) >> 15);

			if (sp->hpf_enabled) {
				x = hpf_process(sp, ch, x);
			}

			/* Band split: cascaded one-pole low-passes,
			 * Q15-extended sample domain.
			 * band[k] = lp[k] - lp[k-1], top band = x - lp[last].
			 */
			int32_t x_q15;

			if (x > (1 << 16)) { /* headroom guard before *2^15 */
				x_q15 = INT32_MAX;
			} else if (x < -(1 << 16)) {
				x_q15 = INT32_MIN;
			} else {
				x_q15 = x * (1 << 15);
			}

			int64_t lf = 0, hf = 0, proxy = 0;
			int32_t prev_lp = 0;

			for (int b = 0; b < SPK_PROTECT_BANDS; b++) {
				int32_t band;

				if (b < SPK_PROTECT_SPLITS) {
					int32_t lp = ch->lp[b] +
						(int32_t)(((int64_t)sp->alpha_q15[b] *
							   (x_q15 - ch->lp[b])) >> 15);

					ch->lp[b] = lp;
					band = lp - prev_lp;
					prev_lp = lp;
				} else {
					band = x_q15 - prev_lp;
				}

				/* Post-static-gain band: what is actually driven */
				int64_t bs = ((int64_t)band * static_gain_q15[b]) >> 15;

				if (b < LF_DUCK_BANDS) {
					lf += bs;
				} else {
					hf += bs;
				}

				/* Sidechain: post-static-gain, current-weighted,
				 * so the budget measures real transducer drive.
				 */
				proxy += (bs * current_weight_q12[b]) >> 12;
			}

			y_lf[c] = lf;
			y_hf[c] = hf;

			/* Q15-extended -> sample domain, rectified. In-phase
			 * transducer currents add, so accumulate |.| across
			 * channels (conservative for uncorrelated content).
			 */
			int64_t p = proxy >> 15;

			frame_proxy += (p < 0) ? -p : p;
		}

		/* --- Sidechain + gain update, once per frame --- */
		sp->env += (frame_proxy * frame_proxy - sp->env) >> sp->env_shift;

		if (sp->env > sp->threshold_env) {
			/* target = sqrt(threshold / env), Q15 */
			int64_t ratio_q30 = (sp->threshold_env << 30) / sp->env;
			int32_t target = (int32_t)isqrt64(ratio_q30);

			if (target < sp->gain_q15) {
				sp->gain_q15 = target;   /* instant attack */
			}
			sp->hold_left = sp->hold_samples;
		} else if (sp->hold_left > 0) {
			sp->hold_left--;
		} else if (sp->gain_q15 < Q15_ONE) {
			int32_t step = (Q15_ONE - sp->gain_q15) >> sp->release_shift;

			sp->gain_q15 += (step > 0) ? step : 1;
		}

		/* --- Recombine with LF-first ducking, write frame out --- */
		int32_t g = sp->gain_q15;
		int32_t g2 = (int32_t)(((int64_t)g * g) >> 15);

		for (int c = 0; c < nch; c++) {
			int64_t y = ((y_lf[c] * g2) >> 15) + y_hf[c];

			y = (y * g) >> 15;      /* broadband gain */
			y >>= 15;               /* Q15-extended -> samples */

			/* Soft-knee clipper, +/-90 % FS knee, 1/4 slope */
			if (y > SOFT_CLIP_KNEE) {
				y = SOFT_CLIP_KNEE +
				    ((y - SOFT_CLIP_KNEE) >> SOFT_CLIP_SHIFT);
			} else if (y < -SOFT_CLIP_KNEE) {
				y = -SOFT_CLIP_KNEE -
				    ((-y - SOFT_CLIP_KNEE) >> SOFT_CLIP_SHIFT);
			}

			pcm[i + c] = sat_i16((int32_t)y);
		}

		/* Advance onset ramp once per frame */
		if (sp->ramp_q15 < Q15_ONE) {
			sp->ramp_q15 += sp->ramp_step;
			if (sp->ramp_q15 > Q15_ONE) {
				sp->ramp_q15 = Q15_ONE;
			}
		}
	}
}
