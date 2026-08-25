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
 * Loudness-rebalanced curve: deepen the cuts below the transducer's
 * efficient region and boost the top band, reallocating the fixed current
 * budget toward the bands that produce audible loudness instead of felt
 * vibration. Blinded on-head A/B against the previous half-depth #180
 * curve: louder at equal buzz, with measured limiter engagement on speech
 * dropping from ~40 % to ~25 % of frames at full volume.
 *
 *   band       < 150  150-275  275-425  425-750  750-1500  > 1500  Hz
 *   #180 curve  -12     -14      -15      -11       -3        0    dB
 *   previous     -6      -7      -7.5     -5.5      -1.5      0    dB
 *   this table  -18     -15     -10       -4        0        +4    dB
 */
static const int32_t static_gain_q15[SPK_PROTECT_BANDS] = {
	4125, 5827, 10362, 20675, 32768, 51934,
};

/* Per-band current weights (Q12, 4096 = 1.0) for the limiter sidechain.
 *
 * Bench-calibrated (PSU + ammeter, tones through the full chain, 3.7 V):
 * once the voicing curve's per-band amplitude differences are divided out,
 * supply current is a function of drive AMPLITUDE alone - flat within a
 * few percent from 150 Hz to 2 kHz, following roughly
 *
 *   I_audio ~= 1550 mA * (output RMS / full scale)^1.8
 *
 * (fit reproduces the bench budget ladder 21/42/72/110 mA audio at budgets
 * 40/60/80/100 and the level ladder). The previous table's "low bands cost
 * 2-3x more" shape was an artifact of the inverted-EQ heuristic: the old
 * voicing attenuated the lows, and per-unit-INPUT current comparisons
 * conflated that with transducer physics.
 *
 * Uniform weights make the sidechain proxy track output amplitude exactly,
 * so the budget caps current honestly for any spectrum - sines, speech and
 * broadband noise alike - at either sample rate. The value 3.0 anchors
 * budget 80 to the bench-measured safe ceiling (~72 mA audio: a sustained
 * sine is clamped to 0.8/3.0 of full scale peak in any band).
 */
static const int32_t current_weight_q12[SPK_PROTECT_BANDS] = {
	12288, 12288, 12288, 12288, 12288, 12288,
};

/* Bands below this index get the limiter gain cubed (3x the dB reduction)
 * so gain reduction lands where the current is drawn: bands 0..2 (< 425 Hz).
 */
#define LF_DUCK_BANDS 3

/* Soft clipper: knee at 90 % FS, 1/4 slope above the knee. */
#define SOFT_CLIP_KNEE  29491
#define SOFT_CLIP_SHIFT 2

/* Psychoacoustic bass enhancer corner frequencies.
 *
 * Source band 60-240 Hz covers the current-hungry region the HPF/EQ removes
 * (and the TTS voice fundamentals). Full-wave rectification of that band
 * generates even harmonics (2f, 4f, ...); the 180 Hz high-pass strips the
 * rectifier's DC and fundamental residue and the 900 Hz low-pass keeps only
 * the 2nd-4th harmonics, which land in the transducer's efficient region.
 * The ear reconstructs the missing fundamental from the harmonic series
 * (the "missing fundamental" effect), so the voice is perceived as full
 * without the transducer being driven at the frequencies that draw current.
 */
#define BASS_SRC_LO_HZ   60
#define BASS_SRC_HI_HZ   240
#define BASS_HARM_LO_HZ  180
#define BASS_HARM_HI_HZ  900

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

static int32_t one_pole_alpha_q15(int sample_rate, int freq_hz)
{
	double omega = 2.0 * M_PI * (double)freq_hz;

	return (int32_t)((double)Q15_ONE * omega /
			 ((double)sample_rate + omega) + 0.5);
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

/** Envelope threshold for a budget percentage (see file header). */
static int64_t threshold_env_for(int budget_percent)
{
	int64_t amp = (32767LL * budget_percent) / 100;

	return (amp * amp) / 2;
}

/** Budget after the runtime drop, floored so audio never fully mutes. */
static int effective_budget(const struct spk_protect *sp)
{
	int budget = sp->params.budget_percent - sp->budget_drop_applied;

	return (budget < 5) ? 5 : budget;
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
	if (params->bass_drive_percent < 0 || params->bass_drive_percent > 200) {
		return -1;
	}

	memset(sp, 0, sizeof(*sp));
	sp->params = *params;

	memcpy(sp->voicing_q15, static_gain_q15, sizeof(sp->voicing_q15));
	memcpy(sp->weight_q12, current_weight_q12, sizeof(sp->weight_q12));
	sp->pregain_q15 = Q15_ONE;

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

	/* --- Psychoacoustic bass enhancer --- */
	if (params->bass_drive_percent > 0) {
		sp->bass_enabled = true;
		sp->bass_drive_q15 = (int32_t)(((int64_t)Q15_ONE *
						params->bass_drive_percent) / 100);
		sp->bass_alpha_lo = one_pole_alpha_q15(params->sample_rate,
						       BASS_SRC_LO_HZ);
		sp->bass_alpha_hi = one_pole_alpha_q15(params->sample_rate,
						       BASS_SRC_HI_HZ);
		sp->bass_alpha_dc = one_pole_alpha_q15(params->sample_rate,
						       BASS_HARM_LO_HZ);
		sp->bass_alpha_out = one_pole_alpha_q15(params->sample_rate,
							BASS_HARM_HI_HZ);
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
	sp->threshold_env = threshold_env_for(params->budget_percent);
	sp->threshold_target = sp->threshold_env;

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
	/* A (re)started stream fades in from silence anyway: land any
	 * pending budget slew instead of carrying it across streams.
	 */
	sp->threshold_env = sp->threshold_target;
	/* stats deliberately survive stream restarts; they clear on init or
	 * an explicit spk_protect_stats_read(reset=true).
	 */
	if (sp->stats.min_gain_q15 == 0) {
		sp->stats.min_gain_q15 = Q15_ONE; /* fresh instance (memset) */
	}
}

void spk_protect_set_voicing(struct spk_protect *sp,
			     const int32_t gains_q15[SPK_PROTECT_BANDS])
{
	if (!sp) {
		return;
	}
	memcpy(sp->voicing_q15, gains_q15 ? gains_q15 : static_gain_q15,
	       sizeof(sp->voicing_q15));
}

void spk_protect_set_weights(struct spk_protect *sp,
			     const int32_t weights_q12[SPK_PROTECT_BANDS])
{
	if (!sp) {
		return;
	}
	memcpy(sp->weight_q12, weights_q12 ? weights_q12 : current_weight_q12,
	       sizeof(sp->weight_q12));
}

void spk_protect_set_pregain(struct spk_protect *sp, int32_t pregain_q15)
{
	if (!sp) {
		return;
	}
	if (pregain_q15 < 0) {
		pregain_q15 = 0;
	}
	if (pregain_q15 > 4 * Q15_ONE) {
		pregain_q15 = 4 * Q15_ONE;
	}
	sp->pregain_q15 = pregain_q15;
}

void spk_protect_set_budget_drop(struct spk_protect *sp, int drop_percent)
{
	if (!sp) {
		return;
	}
	if (drop_percent < 0) {
		drop_percent = 0;
	}
	if (drop_percent > 95) {
		drop_percent = 95;
	}
	/* Single aligned int32 store; the process loop picks it up per
	 * frame and slews the threshold (see spk_protect_process).
	 */
	sp->budget_drop_req = drop_percent;
}

int32_t spk_protect_db10_to_q15(int db10)
{
	double lin = pow(10.0, (double)db10 / 200.0) * (double)Q15_ONE;

	if (lin > 4.0 * Q15_ONE) {
		lin = 4.0 * Q15_ONE;
	}
	return (int32_t)(lin + 0.5);
}

void spk_protect_stats_read(struct spk_protect *sp,
			    struct spk_protect_stats *out, bool reset)
{
	if (!sp || !out) {
		return;
	}
	*out = sp->stats;
	if (reset) {
		sp->stats.min_gain_q15 = Q15_ONE;
		sp->stats.frames = 0;
		sp->stats.limited_frames = 0;
		sp->stats.peak_out = 0;
	}
}

const int32_t *spk_protect_default_voicing(void)
{
	return static_gain_q15;
}

const int32_t *spk_protect_default_weights(void)
{
	return current_weight_q12;
}

/*
 * Psychoacoustic bass: returns the shaped harmonic signal (sample domain)
 * to be mixed into the input BEFORE the HPF removes the source band.
 *
 * Internally Q15-extended for filter accuracy:
 *   bass = LP240^2(x) - LP60(x)        (source band; the upper edge is two
 *                                       cascaded poles, 12 dB/oct, so voice
 *                                       mids stay out of the rectifier)
 *   rect = |bass|                      (even harmonics + DC)
 *   harm = LP900(rect - LP180(rect))   (strip DC/fundamental, cap fizz)
 *   out  = harm * drive
 */
static inline int32_t bass_enhance(const struct spk_protect *sp,
				   struct spk_protect_channel *ch, int32_t x)
{
	int32_t x_q15;

	if (x > 2 * 32767) {
		x = 2 * 32767;
	} else if (x < -2 * 32767) {
		x = -2 * 32767;
	}
	x_q15 = x * (1 << 15);

	ch->bass_lp_lo += (int32_t)(((int64_t)sp->bass_alpha_lo *
				     ((int64_t)x_q15 - ch->bass_lp_lo)) >> 15);
	ch->bass_lp_hi += (int32_t)(((int64_t)sp->bass_alpha_hi *
				     ((int64_t)x_q15 - ch->bass_lp_hi)) >> 15);
	ch->bass_lp_hi2 += (int32_t)(((int64_t)sp->bass_alpha_hi *
				      ((int64_t)ch->bass_lp_hi - ch->bass_lp_hi2)) >> 15);

	int64_t bass = (int64_t)ch->bass_lp_hi2 - ch->bass_lp_lo;
	int32_t rect = (int32_t)((bass < 0) ? -bass : bass);

	ch->bass_lp_dc += (int32_t)(((int64_t)sp->bass_alpha_dc *
				     ((int64_t)rect - ch->bass_lp_dc)) >> 15);

	int32_t harm = rect - ch->bass_lp_dc;

	ch->bass_lp_out += (int32_t)(((int64_t)sp->bass_alpha_out *
				      ((int64_t)harm - ch->bass_lp_out)) >> 15);

	int64_t out = ((int64_t)ch->bass_lp_out * sp->bass_drive_q15) >> 15;

	return (int32_t)(out >> 15); /* Q15-extended -> sample domain */
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

			/* Digital pre-gain (may exceed unity; the limiter
			 * sidechain below sees the boosted level, so the
			 * energy budget still holds).
			 */
			if (sp->pregain_q15 != Q15_ONE) {
				x = (int32_t)(((int64_t)x * sp->pregain_q15) >> 15);
			}

			/* Harmonics are generated from (and mixed into) the
			 * pre-HPF signal, so the added energy is itself
			 * protection-limited by the chain below.
			 */
			if (sp->bass_enabled) {
				x += bass_enhance(sp, ch, x);
			}

			if (sp->hpf_enabled) {
				x = hpf_process(sp, ch, x);
			}

			/* Band split: cascaded one-pole low-passes,
			 * Q15-extended sample domain.
			 * band[k] = lp[k] - lp[k-1], top band = x - lp[last].
			 */
			int32_t x_q15;

			/* Headroom guard before *2^15: saturate at +/- 2x FS
			 * (pre-gain and HPF overshoot can exceed int16), so
			 * x_q15 and the split arithmetic below stay in range.
			 */
			if (x > 2 * 32767) {
				x = 2 * 32767;
			} else if (x < -2 * 32767) {
				x = -2 * 32767;
			}
			x_q15 = x * (1 << 15);

			int64_t lf = 0, hf = 0, proxy = 0;
			int32_t prev_lp = 0;

			for (int b = 0; b < SPK_PROTECT_BANDS; b++) {
				int64_t band;

				if (b < SPK_PROTECT_SPLITS) {
					int32_t lp = ch->lp[b] +
						(int32_t)(((int64_t)sp->alpha_q15[b] *
							   ((int64_t)x_q15 - ch->lp[b])) >> 15);

					ch->lp[b] = lp;
					band = (int64_t)lp - prev_lp;
					prev_lp = lp;
				} else {
					band = (int64_t)x_q15 - prev_lp;
				}

				/* Post-static-gain band: what is actually driven */
				int64_t bs = (band * sp->voicing_q15[b]) >> 15;

				if (b < LF_DUCK_BANDS) {
					lf += bs;
				} else {
					hf += bs;
				}

				/* Sidechain: post-static-gain, current-weighted,
				 * so the budget measures real transducer drive.
				 */
				proxy += (bs * sp->weight_q12[b]) >> 12;
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

		/* --- Budget slew (display-aware ducking), once per frame --- */
		if (sp->budget_drop_req != sp->budget_drop_applied) {
			sp->budget_drop_applied = sp->budget_drop_req;
			sp->threshold_target =
				threshold_env_for(effective_budget(sp));
			if (sp->ramp_q15 < Q15_ONE) {
				/* stream still fading in: land it now */
				sp->threshold_env = sp->threshold_target;
			} else {
				int32_t frames = Q15_ONE / sp->ramp_step;
				int64_t delta = sp->threshold_target -
						sp->threshold_env;
				int64_t step = delta / (frames > 0 ? frames : 1);

				if (step == 0) {
					step = (delta > 0) ? 1 : -1;
				}
				sp->threshold_step = step;
			}
		}
		if (sp->threshold_env != sp->threshold_target) {
			sp->threshold_env += sp->threshold_step;
			if ((sp->threshold_step > 0 &&
			     sp->threshold_env > sp->threshold_target) ||
			    (sp->threshold_step < 0 &&
			     sp->threshold_env < sp->threshold_target)) {
				sp->threshold_env = sp->threshold_target;
			}
		}

		/* --- Sidechain + gain update, once per frame --- */
		sp->env += (frame_proxy * frame_proxy - sp->env) >> sp->env_shift;

		if (sp->env > sp->threshold_env) {
			/* target = sqrt(threshold / env), Q15 */
			int64_t ratio_q30 = (sp->threshold_env << 30) / sp->env;
			int32_t target = (int32_t)isqrt64(ratio_q30);

			if (target < sp->gain_q15) {
				sp->gain_q15 = target;   /* instant attack */
			} else if (sp->gain_q15 < target) {
				/* Clamped harder than the current threshold
				 * requires - happens when the budget is raised
				 * mid-clamp (display back to power save).
				 * Release toward the exact safe clamp; without
				 * this the gain would ratchet down for as long
				 * as the content stays over budget.
				 */
				int32_t step = (target - sp->gain_q15) >>
					       sp->release_shift;

				sp->gain_q15 += (step > 0) ? step : 1;
				if (sp->gain_q15 > target) {
					sp->gain_q15 = target;
				}
			}
			sp->hold_left = sp->hold_samples;
		} else if (sp->hold_left > 0) {
			sp->hold_left--;
		} else if (sp->gain_q15 < Q15_ONE) {
			int32_t step = (Q15_ONE - sp->gain_q15) >> sp->release_shift;

			sp->gain_q15 += (step > 0) ? step : 1;
		}

		sp->stats.frames++;
		if (sp->gain_q15 < Q15_ONE) {
			sp->stats.limited_frames++;
			if (sp->gain_q15 < sp->stats.min_gain_q15) {
				sp->stats.min_gain_q15 = sp->gain_q15;
			}
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

			int32_t ay = (int32_t)((y < 0) ? -y : y);

			if (ay > sp->stats.peak_out) {
				sp->stats.peak_out = ay;
			}
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
