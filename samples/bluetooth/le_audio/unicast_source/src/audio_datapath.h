/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
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

#define I2S_NODE   DT_ALIAS(i2s_bus)
#define CODEC_NODE DT_ALIAS(audio_codec)

typedef int (*audio_datapath_channel_create_t)(size_t octets_per_frame, uint8_t stream_lid);

typedef int (*audio_datapath_start_stop_channel_t)(uint8_t stream_lid);

/**
 * @brief Audio datapath configuration structure
 *
 * This structure is used to configure the audio datapath
 */
struct audio_datapath_config {
	/** Presentation delay in microseconds */
	uint32_t pres_delay_us;
	/** Sampling rate in Hz */
	uint32_t sampling_rate_hz;
	/** Frame duration is 10ms */
	bool frame_duration_is_10ms;
};

/**
 * @brief Create the audio source datapath
 *
 * This creates and configures all of the required elements to stream audio from Bluetooth LE to I2S
 *
 * @param cfg Desired configuration of the audio datapath
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_datapath_create_source(struct audio_datapath_config const *cfg);

/**
 * @brief Create the audio source datapath channel
 *
 * @param[in] octets_per_frame Number of data per frame
 * @param[in] stream_lid Stream local ID of the channel to be created
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_datapath_channel_create_source(size_t octets_per_frame, uint8_t stream_lid);

/**
 * @brief Start the audio source datapath
 *
 * Starts transmission of the first SDU over the ISO datapath,
 * which when transmitted starts off the operation of the rest of
 * the datapath.
 *
 * @param[in] stream_lid Stream local ID
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_datapath_channel_start_source(uint8_t stream_lid);

/**
 * @brief Stop the audio source datapath
 *
 * Stops transmission of the SDUs over the ISO datapath.
 *
 * @param[in] stream_lid Stream local ID
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_datapath_channel_stop_source(uint8_t stream_lid);

/**
 * @brief Clean up the audio source datapath
 *
 * This stops the audio source datapath and cleans up all elements created by
 * audio_datapath_create_source, freeing any allocated memory if necessary. After calling this
 * function, it is possible to create a new audio source datapath again using
 * audio_datapath_create_source.
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_datapath_cleanup_source(void);

/**
 * @brief Create the audio sink datapath
 *
 * This creates and configures all of the required elements to stream audio from I2S to Bluetooth LE
 *
 * @param cfg Desired configuration of the audio datapath
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_datapath_create_sink(struct audio_datapath_config const *cfg);

/**
 * @brief Create the audio sink datapath channel
 *
 * @param[in] octets_per_frame Number of data per frame
 * @param[in] stream_lid Stream local ID of the channel to be created
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_datapath_channel_create_sink(size_t octets_per_frame, uint8_t stream_lid);

/**
 * @brief Start the audio sink datapath channel
 *
 * Starts transmission of the first SDU over the ISO datapath,
 * which when transmitted starts off the operation of the rest of
 * the datapath.
 *
 * @param[in] stream_lid Stream local ID
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_datapath_channel_start_sink(uint8_t stream_lid);

/**
 * @brief Stop the audio sink datapath channel
 *
 * Stops transmission of the SDUs over the ISO datapath.
 *
 * @param[in] stream_lid Stream local ID
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_datapath_channel_stop_sink(uint8_t stream_lid);

/**
 * @brief Clean up the audio sink datapath
 *
 * This stops the audio sink datapath and cleans up all elements created by
 * audio_datapath_create_sink, freeing any allocated memory if necessary. After calling this
 * function, it is possible to create a new audio sink datapath again using
 * audio_datapath_create_sink.
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_datapath_cleanup_sink(void);

#endif /* _AUDIO_DATAPATH_H */
