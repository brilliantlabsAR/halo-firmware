/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef MIC_SOURCE_H
#define MIC_SOURCE_H

/**
 * @brief Configure microphone source using I2S
 *
 * @param dev I2S device to use
 * @param audio_queue Audio queue that data will be sent to
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int mic_i2s_configure(const struct device *dev, struct audio_queue *audio_queue);

/**
 * @brief Start the microphone source stream
 */
void mic_i2s_start(void);

/**
 * @brief Stop the microphone source stream
 */
void mic_i2s_stop(void);

#endif /* MIC_SOURCE_H */
