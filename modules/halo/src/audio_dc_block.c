/*
 * Copyright (c) 2026 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/sys/util.h>

#include <halo/audio_dc_block.h>

/* The PDM decimator runs with its IIR bypassed (halo.dts iir-bypass), which
 * leaves a large persistent DC bias (~1265 counts) on the mic.
 *
 *   y[n] = x[n] - x[n-1] + R * y[n-1]   R=0.997 -> ~7.6Hz corner @16kHz,
 * far below any voice content. Per-channel state, interleaved buffer.
 */
#define DCB_R 0.997f
/* Start-up: the mic's DC level settles for ~200 ms after a start (it begins
 * ~10000 counts at gain 0 above its final value and decays with a ~40 ms time
 * constant). A 3.8-7.6 Hz blocker primed to the first sample follows that
 * decay into a ~5000-count, ~100 ms "plateau" at the start of every capture.
 * So the blocker is two cascaded one-pole stages that start at a corner of
 * DCB_START_HZ, hold it for DCB_HOLD_MS, then ramp to DCB_R over DCB_RAMP_MS.
 * Measured on raw captures: the start-up excursion drops from ~5000 to ~300
 * counts, gone within the first 20 ms block.
 */
#define DCB_START_HZ 80.0f
#define DCB_HOLD_MS  100
#define DCB_RAMP_MS  100

void audio_dc_block_reset(struct audio_dc_block *dcb)
{
	for (int st = 0; st < 2; st++) {
		for (int ch = 0; ch < AUDIO_DC_BLOCK_MAX_CHANNELS; ch++) {
			dcb->yprev[st][ch] = 0.0f;
			dcb->xprev[st][ch] = 0.0f;
		}
	}
	dcb->frames = 0;
	dcb->primed = false;
}

/* Pole for the frame `n` frames after a start (see DCB_START_HZ) */
static float dcb_pole(uint32_t n, int fs)
{
	uint32_t hold = (uint32_t)fs * DCB_HOLD_MS / 1000;
	uint32_t ramp = (uint32_t)fs * DCB_RAMP_MS / 1000;
	float r_start = 1.0f - 6.2831853f * DCB_START_HZ / (float)fs;

	if (n < hold) {
		return r_start;
	}
	if (n >= hold + ramp) {
		return DCB_R;
	}
	float k = (float)(n - hold) / (float)ramp;

	return r_start + (DCB_R - r_start) * k;
}

void audio_dc_block_process(struct audio_dc_block *dcb, int16_t *buf, size_t num_samples,
			    int channels, int fs)
{
	if (channels < 1 || channels > AUDIO_DC_BLOCK_MAX_CHANNELS || fs <= 0) {
		return;
	}
	/* Warm-start: on the first block after a start, prime xprev to that
	 * block's first sample per channel so the blocker emits 0 for the initial
	 * DC level and only tracks CHANGES from it. Starting from xprev=0 instead
	 * differentiates the start-up DC step into a transient (an audible
	 * start-up "tick", and a band-pass downstream rings on it). Interleaved,
	 * so the first `channels` samples are the first sample of each channel. */
	if (!dcb->primed && num_samples >= (size_t)channels) {
		for (int c = 0; c < channels; c++) {
			dcb->xprev[0][c] = (float)buf[c];
			dcb->xprev[1][c] = 0.0f;
		}
		dcb->primed = true;
	}
	uint32_t settled = (uint32_t)fs * (DCB_HOLD_MS + DCB_RAMP_MS) / 1000;
	float r = dcb_pole(dcb->frames, fs);
	int ch = 0;

	for (size_t i = 0; i < num_samples; i++) {
		float v = (float)buf[i];

		for (int st = 0; st < 2; st++) {
			float y = v - dcb->xprev[st][ch] + r * dcb->yprev[st][ch];

			dcb->xprev[st][ch] = v;
			dcb->yprev[st][ch] = y;
			v = y;
		}
		int32_t yi = (int32_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);

		buf[i] = (int16_t)CLAMP(yi, INT16_MIN, INT16_MAX);
		if (++ch >= channels) {
			ch = 0;
			if (dcb->frames < settled) {
				dcb->frames++;
				r = dcb_pole(dcb->frames, fs);
			}
		}
	}
}
