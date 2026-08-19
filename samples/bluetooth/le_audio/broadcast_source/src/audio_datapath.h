/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef AUDIO_DATAPATH_H_
#define AUDIO_DATAPATH_H_

#include <zephyr/types.h>
/**
 * @brief Initializes the audio datapath
 *
 * This creates and configures all of the required elements to stream audio from Bluetooth LE to I2S
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_datapath_init(void);

/**
 * @brief Creates the audio source datapath channel
 *
 * @param octets_per_frame Number of data per frame
 * @param stream_lid Stream local ID of the channel to be created
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_datapath_channel_create(size_t octets_per_frame, uint8_t stream_lid);

/**
 * @brief Starts the audio datapath
 *
 * This starts reception of the first SDU over the ISO datapath, which when received starts off the
 * operation of the rest of the datapath.
 *
 * @param stream_lid Stream local ID of the channel to be started
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
void audio_datapath_start(uint8_t stream_lid);

/**
 * @brief Controls the microphone input
 *
 * @param start True to start the microphone input, false to stop it
 */
void audio_datapath_mic_control(bool start);

#endif /* AUDIO_DATAPATH_H_ */
