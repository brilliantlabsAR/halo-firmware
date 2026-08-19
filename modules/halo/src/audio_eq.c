/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file audio_eq.c
 * @brief Biquad IIR high-pass filter for bone conduction speaker protection.
 *
 * Implements a Butterworth high-pass filter using fixed-point Q30 arithmetic.
 * Designed to attenuate low-frequency content that causes excessive current
 * draw and vibration in bone conduction speakers.
 *
 * The filter uses Direct Form II Transposed structure for numerical stability
 * and processes int16 PCM data in-place.
 */

#include <halo/audio_eq.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <math.h>

/* M_PI is a POSIX/GNU extension and is not exposed by <math.h> under
 * strict C99 (-std=c99), which Zephyr uses. Define locally if missing.
 */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

LOG_MODULE_REGISTER(audio_eq, CONFIG_HALO_LOG_LEVEL);

/** Q30 fixed-point scale factor: 1.0 = (1 << 30) = 1073741824 */
#define Q30_SHIFT   30
#define Q30_ONE     (1 << Q30_SHIFT)

/** Convert float to Q30 fixed-point */
#define FLOAT_TO_Q30(f) ((int32_t)((f) * (double)Q30_ONE + ((f) >= 0 ? 0.5 : -0.5)))

/** Clamp int64 accumulator to int16 range */
static inline int16_t saturate_i16(int64_t val)
{
	if (val > INT16_MAX) {
		return INT16_MAX;
	}
	if (val < INT16_MIN) {
		return INT16_MIN;
	}
	return (int16_t)val;
}

/**
 * @brief Compute Butterworth HPF biquad coefficients for one 2nd-order section.
 *
 * For a single section (2nd-order Butterworth):
 *   Q = 0.7071 (1/sqrt(2))
 *
 * For cascaded 4th-order (2 sections), the Q values are:
 *   Section 0: Q = 0.5412 (cos(5*pi/8) related)
 *   Section 1: Q = 1.3066 (cos(7*pi/8) related)
 *
 * Transfer function (s-domain HPF):
 *   H(s) = s^2 / (s^2 + s/Q + 1)
 *
 * Bilinear transform with pre-warping applied.
 */
static void compute_hpf_coeffs(struct audio_biquad_coeffs *coeffs,
				int sample_rate, int cutoff_hz, double Q)
{
	double w0 = 2.0 * M_PI * (double)cutoff_hz / (double)sample_rate;
	double cos_w0 = cos(w0);
	double sin_w0 = sin(w0);
	double alpha = sin_w0 / (2.0 * Q);

	/* HPF coefficients (before normalization by a0) */
	double b0 = (1.0 + cos_w0) / 2.0;
	double b1 = -(1.0 + cos_w0);
	double b2 = (1.0 + cos_w0) / 2.0;
	double a0 = 1.0 + alpha;
	double a1 = -2.0 * cos_w0;
	double a2 = 1.0 - alpha;

	/* Normalize by a0 and convert to Q30 */
	coeffs->b0 = FLOAT_TO_Q30(b0 / a0);
	coeffs->b1 = FLOAT_TO_Q30(b1 / a0);
	coeffs->b2 = FLOAT_TO_Q30(b2 / a0);
	coeffs->a1 = FLOAT_TO_Q30(a1 / a0);
	coeffs->a2 = FLOAT_TO_Q30(a2 / a0);

	LOG_DBG("HPF coeffs (Q30): b0=%d b1=%d b2=%d a1=%d a2=%d",
		coeffs->b0, coeffs->b1, coeffs->b2, coeffs->a1, coeffs->a2);
}

int audio_hpf_init(struct audio_hpf *hpf, int sample_rate, int cutoff_hz,
		   int num_channels, int order)
{
	if (!hpf) {
		return -EINVAL;
	}

	if (num_channels < 1 || num_channels > 2) {
		LOG_ERR("Invalid channel count: %d (must be 1 or 2)", num_channels);
		return -EINVAL;
	}

	if (order != 2 && order != 4) {
		LOG_ERR("Invalid filter order: %d (must be 2 or 4)", order);
		return -EINVAL;
	}

	if (cutoff_hz <= 0 || cutoff_hz >= sample_rate / 2) {
		LOG_ERR("Invalid cutoff %d Hz for sample rate %d Hz", cutoff_hz, sample_rate);
		return -EINVAL;
	}

	memset(hpf, 0, sizeof(*hpf));
	hpf->num_channels = num_channels;
	hpf->enabled = true;

	if (order == 2) {
		/* Single 2nd-order Butterworth section, Q = 1/sqrt(2) */
		hpf->num_stages = 1;
		compute_hpf_coeffs(&hpf->coeffs[0], sample_rate, cutoff_hz, 0.7071067811865476);
	} else {
		/* 4th-order Butterworth: cascade two 2nd-order sections
		 * Q values for 4th-order Butterworth:
		 *   Stage 0: Q = 1 / (2 * cos(3*pi/8)) = 0.54119610
		 *   Stage 1: Q = 1 / (2 * cos(1*pi/8)) = 1.30656296
		 */
		hpf->num_stages = 2;
		compute_hpf_coeffs(&hpf->coeffs[0], sample_rate, cutoff_hz, 0.54119610);
		compute_hpf_coeffs(&hpf->coeffs[1], sample_rate, cutoff_hz, 1.30656296);
	}

	LOG_INF("HPF initialized: %d Hz cutoff, %d Hz sample rate, order %d, %d ch",
		cutoff_hz, sample_rate, order, num_channels);

	return 0;
}

void audio_hpf_reset(struct audio_hpf *hpf)
{
	if (!hpf) {
		return;
	}

	for (int ch = 0; ch < hpf->num_channels; ch++) {
		for (int s = 0; s < hpf->num_stages; s++) {
			hpf->channels[ch].stages[s].z1 = 0;
			hpf->channels[ch].stages[s].z2 = 0;
		}
	}
}

/**
 * @brief Process one sample through a single biquad stage.
 *
 * Direct Form II Transposed:
 *   y[n] = b0*x[n] + z1
 *   z1   = b1*x[n] - a1*y[n] + z2
 *   z2   = b2*x[n] - a2*y[n]
 *
 * All multiplications use int64 to avoid overflow.
 * State variables (z1, z2) are kept in Q30-scaled domain (Q30 * int16 = Q30+15 range).
 */
static inline int16_t biquad_process_sample(const struct audio_biquad_coeffs *c,
					    struct audio_biquad_state *s,
					    int16_t input)
{
	int64_t x = (int64_t)input;
	int64_t y;

	/* y = b0 * x + z1 (z1 is already in Q30-shifted domain) */
	y = ((int64_t)c->b0 * x + s->z1) >> Q30_SHIFT;

	/* Update state */
	s->z1 = (int64_t)c->b1 * x - (int64_t)c->a1 * y + s->z2;
	s->z2 = (int64_t)c->b2 * x - (int64_t)c->a2 * y;

	return saturate_i16(y);
}

void audio_hpf_process(struct audio_hpf *hpf, int16_t *pcm, size_t num_samples)
{
	if (!hpf || !hpf->enabled || !pcm || num_samples == 0) {
		return;
	}

	int nch = hpf->num_channels;
	int nstages = hpf->num_stages;

	for (size_t i = 0; i < num_samples; i++) {
		int ch = (nch > 1) ? (i % nch) : 0;
		int16_t sample = pcm[i];

		/* Cascade through all biquad stages */
		for (int s = 0; s < nstages; s++) {
			sample = biquad_process_sample(&hpf->coeffs[s],
						       &hpf->channels[ch].stages[s],
						       sample);
		}

		pcm[i] = sample;
	}
}

void audio_hpf_set_enabled(struct audio_hpf *hpf, bool enable)
{
	if (!hpf) {
		return;
	}

	if (hpf->enabled != enable) {
		hpf->enabled = enable;
		if (enable) {
			/* Reset state on re-enable to avoid transient */
			audio_hpf_reset(hpf);
		}
		LOG_INF("HPF %s", enable ? "enabled" : "disabled");
	}
}
