/*
 * Copyright (c) 2026 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_AUDIO_DC_BLOCK_H_
#define HALO_AUDIO_DC_BLOCK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_DC_BLOCK_MAX_CHANNELS 2

/**
 * @brief Microphone DC blocker with a fast start-up corner
 *
 * The PDM decimator runs with its IIR bypassed, so the mic carries a large DC
 * bias, and after every start that bias settles for about 200 ms. This removes
 * both: two cascaded one-pole high-pass stages that start at a high corner to
 * follow the settling, then slow to a corner far below voice content. See
 * audio_dc_block.c for the numbers.
 *
 * One instance per capture path. Not thread-safe: reset and process from the
 * same thread, or reset while that thread is not processing.
 */
struct audio_dc_block {
	float xprev[2][AUDIO_DC_BLOCK_MAX_CHANNELS];
	float yprev[2][AUDIO_DC_BLOCK_MAX_CHANNELS];
	uint32_t frames; /* frames since start, saturating */
	bool primed;
};

/** @brief Reset for a new capture; call on every mic start. */
void audio_dc_block_reset(struct audio_dc_block *dcb);

/**
 * @brief Filter an interleaved int16 buffer in place
 *
 * @param num_samples total samples (frames * channels)
 * @param channels    1..AUDIO_DC_BLOCK_MAX_CHANNELS; otherwise a no-op
 * @param fs          sample rate in Hz
 */
void audio_dc_block_process(struct audio_dc_block *dcb, int16_t *buf, size_t num_samples,
			    int channels, int fs);

#ifdef __cplusplus
}
#endif

#endif /* HALO_AUDIO_DC_BLOCK_H_ */
