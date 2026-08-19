/* Copyright Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef _AUDIO_DATAPATH_H
#define _AUDIO_DATAPATH_H

#include <zephyr/types.h>
#include <zephyr/device.h>
#include <stdbool.h>

struct audio_datapath_config {
	const struct device *i2s_dev;
	/* const struct device *mclk_dev; */
	uint32_t pres_delay_us;
	uint32_t sampling_rate_hz;
	uint16_t octets_per_frame;
	bool frame_duration_is_10ms;
};

/**
 * @brief Create the audio datapath for sink
 *
 * Creates and configures all of the required elements to stream audio from Bluetooth LE to I2S.
 * This includes configuring and starting the codec.
 *
 * @param cfg Desired configuration of the audio datapath
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_datapath_create_sink(struct audio_datapath_config const *cfg);

/**
 * @brief Create a channel for the audio datapath
 *
 * Creates a channel for the audio decoder.
 *
 * @param octets_per_frame Number of octets per frame
 * @param ch_index Channel index
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_datapath_channel_create_sink(size_t const octets_per_frame, uint8_t const ch_index);

/**
 * @brief Start a channel for the audio datapath
 *
 * Starts a channel for the audio decoder.
 *
 * @param ch_index Channel index
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_datapath_channel_start_sink(uint8_t const ch_index);

/**
 * @brief Start the audio datapath
 *
 * Starts reception of the first SDU over the ISO datapath, which when received starts off the
 * operation of the rest of the datapath.
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_datapath_start_sink(void);

/**
 * @brief Clean up the audio datapath
 *
 * Stops the audio datapath and cleans up all elements created by audio_datapath_create_sink,
 * freeing any allocated memory if necessary. After calling this function, it is possible to create
 * a new audio datapath again using audio_datapath_create_sink.
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_datapath_cleanup_sink(void);

/**
 * @brief Create the audio datapath for source
 *
 * Creates and configures all of the required elements to stream audio from I2S to Bluetooth LE.
 * This includes configuring and starting the codec input.
 *
 * @param cfg Desired configuration of the audio datapath
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_datapath_create_source(struct audio_datapath_config const *cfg);

/**
 * @brief Add a channel to the audio encoder
 *
 * Adds a channel to the audio encoder for source streaming.
 *
 * @param octets_per_frame Number of octets per frame
 * @param stream_lid Stream local ID
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_datapath_channel_create_source(size_t octets_per_frame, uint8_t stream_lid);

/**
 * @brief Start a channel for the audio datapath source
 *
 * Starts a channel for the audio encoder.
 *
 * @param stream_lid Stream local ID
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_datapath_channel_start_source(uint8_t stream_lid);

/**
 * @brief Clean up the audio datapath for source
 *
 * Stops and cleans up all source audio datapath elements.
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_datapath_cleanup_source(void);

#endif /* _AUDIO_DATAPATH_H */
