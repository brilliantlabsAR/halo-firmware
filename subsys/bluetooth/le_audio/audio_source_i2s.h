/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef _AUDIO_SOURCE_I2S_H
#define _AUDIO_SOURCE_I2S_H

#include <zephyr/types.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include "audio_queue.h"

/**
 * @brief Configure audio source using I2S
 *
 * @param dev I2S device to use
 * @param audio_queue Audio queue that data will be sent to
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_source_i2s_configure(const struct device *dev, struct audio_queue *audio_queue);

/**
 * @brief Start the audio source stream
 */
void audio_source_i2s_start(void);

/**
 * @brief Stop the audio source stream
 */
void audio_source_i2s_stop(void);

/**
 * @brief Apply a timing correction to the audio source
 *
 * @param correction_us The length of time in microseconds to correct by. A positive number
 * indicates that additional audio samples should be added, and a negative number indicates that
 * audio samples should be dropped.
 */
void audio_source_i2s_apply_timing_correction(int32_t correction_us);

#endif /* _AUDIO_SOURCE_I2S_H */
