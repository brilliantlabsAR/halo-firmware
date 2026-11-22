/*
 * Copyright (c) 2026 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <halo/audio_voice_band.h>

#include <math.h>
#include <string.h>

/* 4th-order Butterworth = two 2nd-order sections with these section Q's */
static const float voice_q[AUDIO_VOICE_BAND_NSEC] = { 0.54119610f, 1.30656296f };

static void biquad_lowpass(struct audio_voice_biquad *bq, float fc, float fs, float q)
{
	float w0 = 6.2831853072f * fc / fs; /* 2*pi*fc/fs */
	float c = cosf(w0), s = sinf(w0);
	float alpha = s / (2.0f * q);
	float a0 = 1.0f + alpha;

	bq->b0 = ((1.0f - c) * 0.5f) / a0;
	bq->b1 = (1.0f - c) / a0;
	bq->b2 = bq->b0;
	bq->a1 = (-2.0f * c) / a0;
	bq->a2 = (1.0f - alpha) / a0;
}

static void biquad_highpass(struct audio_voice_biquad *bq, float fc, float fs, float q)
{
	float w0 = 6.2831853072f * fc / fs; /* 2*pi*fc/fs */
	float c = cosf(w0), s = sinf(w0);
	float alpha = s / (2.0f * q);
	float a0 = 1.0f + alpha;

	bq->b0 = ((1.0f + c) * 0.5f) / a0;
	bq->b1 = (-(1.0f + c)) / a0;
	bq->b2 = bq->b0;
	bq->a1 = (-2.0f * c) / a0;
	bq->a2 = (1.0f - alpha) / a0;
}

static inline float biquad_step(const struct audio_voice_biquad *bq, float x, float *z1,
				float *z2)
{
	float y = bq->b0 * x + *z1; /* Direct Form II transposed */

	*z1 = bq->b1 * x - bq->a1 * y + *z2;
	*z2 = bq->b2 * x - bq->a2 * y;
	return y;
}

void audio_voice_band_init(struct audio_voice_band *vb, int sample_rate)
{
	float fs = (float)sample_rate;

	memset(vb, 0, sizeof(*vb));
	for (int k = 0; k < AUDIO_VOICE_BAND_NSEC; k++) {
		biquad_highpass(&vb->hp[k], AEC_VOICE_HP_HZ, fs, voice_q[k]);
		biquad_lowpass(&vb->lp[k], AEC_VOICE_LP_HZ, fs, voice_q[k]);
	}
}

void audio_voice_band_process(struct audio_voice_band *vb, int16_t *pcm, size_t samples,
			      int channels)
{
	if (channels < 1 || channels > AUDIO_VOICE_BAND_CHANNELS) {
		return;
	}
	int ch = 0;

	for (size_t i = 0; i < samples; i++) {
		float x = (float)pcm[i];

		for (int k = 0; k < AUDIO_VOICE_BAND_NSEC; k++) {
			x = biquad_step(&vb->hp[k], x, &vb->hp_z1[k][ch], &vb->hp_z2[k][ch]);
		}
		for (int k = 0; k < AUDIO_VOICE_BAND_NSEC; k++) {
			x = biquad_step(&vb->lp[k], x, &vb->lp_z1[k][ch], &vb->lp_z2[k][ch]);
		}

		int32_t y = (int32_t)(x >= 0.0f ? x + 0.5f : x - 0.5f);

		if (y > INT16_MAX) {
			y = INT16_MAX;
		} else if (y < INT16_MIN) {
			y = INT16_MIN;
		}
		pcm[i] = (int16_t)y;
		if (++ch >= channels) {
			ch = 0;
		}
	}
}
