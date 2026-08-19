/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef _AUDIO_I2S_COMMON_H
#define _AUDIO_I2S_COMMON_H

#include <zephyr/kernel.h>

struct audio_i2s_timing {
	struct k_spinlock lock;
	int32_t correction_us;
	uint32_t us_per_block;
	uint32_t samples_per_block;
	int32_t max_single_correction;
	int32_t min_single_correction;
};

/**
 * @brief Apply a timing correction to audio data
 *
 * @param timing Context struct to update
 * @param correction_us Correction to apply in microseconds
 *
 * @retval 0 on success
 * @retval Negative error code on failure
 */
int audio_i2s_timing_apply_correction(struct audio_i2s_timing *timing, int32_t correction_us);

/**
 * @brief Get the correction which should be applied to the next audio block
 *
 * @param timing Context struct to get timing correction from
 *
 * @retval Number of samples to correct by. A positive value indicates additional samples should be
 * added. A negative value indicates samples should be dropped.
 */
int32_t audio_i2s_get_sample_correction(struct audio_i2s_timing *timing);

/**
 * @brief Convert microseconds to audio samples
 */
static inline int32_t audio_i2s_us_to_samples(int32_t us, int32_t us_per_block,
					      int32_t samples_per_block)
{
	return (samples_per_block * us) / us_per_block;
}

/**
 * @brief Convert audio samples to microseconds
 */
static inline int32_t audio_i2s_samples_to_us(int32_t samples, int32_t us_per_block,
					      int32_t samples_per_block)
{
	return (samples * us_per_block) / samples_per_block;
}

#endif /* _AUDIO_I2S_COMMON_H */
