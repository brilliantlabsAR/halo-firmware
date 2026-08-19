/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/sys/util.h>
#include "audio_i2s_common.h"

#if CONFIG_ALIF_BLE_AUDIO_USE_RAMFUNC
#define INT_RAMFUNC __ramfunc
#else
#define INT_RAMFUNC
#endif

INT_RAMFUNC int audio_i2s_timing_apply_correction(struct audio_i2s_timing *timing,
						  int32_t const correction_us)
{
	if (timing == NULL) {
		return -EINVAL;
	}

	/* Don't allow a correction of more than one block to be applied in one time step */
	int32_t adjusted_correction = correction_us;

	adjusted_correction = MIN(correction_us, timing->us_per_block);
	adjusted_correction = MAX(correction_us, -((int32_t)timing->us_per_block));

	/* Keep cumulative correction within limits that prevent integer overflow when later
	 * converting microseconds to samples. In reality the correction should not get close to the
	 * limits, as presentation delay is usually of the order of ~40000 us.
	 */
	const int32_t max = INT32_MAX / (int32_t)timing->samples_per_block;
	const int32_t min = INT32_MIN / (int32_t)timing->samples_per_block;

	k_spinlock_key_t key = k_spin_lock(&timing->lock);

	timing->correction_us += adjusted_correction;
	timing->correction_us = MIN(timing->correction_us, max);
	timing->correction_us = MAX(timing->correction_us, min);

	k_spin_unlock(&timing->lock, key);

	return 0;
}

INT_RAMFUNC int32_t audio_i2s_get_sample_correction(struct audio_i2s_timing *timing)
{
	if (timing == NULL) {
		return 0;
	}

	k_spinlock_key_t key = k_spin_lock(&timing->lock);

	/* The correction must be an even number of samples (in case of stereo audio) */
	int32_t correction_samples =
		audio_i2s_us_to_samples(timing->correction_us, timing->us_per_block,
					timing->samples_per_block) &
		~0x1UL;

	correction_samples = MIN(correction_samples, timing->max_single_correction);
	correction_samples = MAX(correction_samples, timing->min_single_correction);

	/* Subtract correction to be applied from the cumulative correction count */
	timing->correction_us -= audio_i2s_samples_to_us(correction_samples, timing->us_per_block,
							 timing->samples_per_block);

	k_spin_unlock(&timing->lock, key);

	return correction_samples;
}
