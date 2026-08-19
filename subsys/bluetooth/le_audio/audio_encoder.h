/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef _AUDIO_ENCODER_H
#define _AUDIO_ENCODER_H

#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include "bluetooth/le_audio/audio_queue.h"
#include "bluetooth/le_audio/sdu_queue.h"

struct audio_encoder_params {
	const struct device *i2s_dev;
	uint32_t audio_buffer_len_us;
	uint32_t frame_duration_us;
	uint32_t sampling_rate_hz;
	size_t num_queues;
	struct sdu_queue *p_sdu_queues[];
};

/**
 * @brief Callback function signature for SDU completion
 *
 * @param context User-defined context to be passed to callback
 * @param capture_timestamp ISO clock time at which audio block contained in SDU was captured
 * @param sdu_seq Sequence number of SDU
 */
typedef void (*audio_encoder_sdu_cb_t)(void *context, uint32_t capture_timestamp, uint16_t sdu_seq);

/**
 * @brief Create and start an audio encoder instance
 *
 * The audio encoder instance waits on audio data to be available in the provided audio queue, and
 * then encodes the data using the LC3 codec before pushing it to the SDU queue(s). It supports
 * either single or dual channel encoding.
 *
 * @note The stack for the encoder thread is currently passed in as a parameter since at the time of
 * writing, the targeted Zephyr version does not support dynamically allocating thread stacks. It
 * would be much better to dynamically allocate the stack, as this means the correct stacksize can
 * be set internally to the module and the user does not need to know this. This PR:
 * https://github.com/zephyrproject-rtos/zephyr/pull/44379 adds the functionality needed, so this
 * module should be update to use dynamically allocated stacks at the point where the targeted
 * Zephyr version is updated to include this feature.
 *
 * @param params Audio encoder configuration parameters
 *
 * @retval Created audio encoder instance if successful
 * @retval NULL on failure
 */
struct audio_encoder *audio_encoder_create(struct audio_encoder_params const *params);

/**
 * @brief Set the audio queue for the audio encoder
 *
 * @param encoder Audio encoder instance to set audio queue for
 *
 * @retval Audio queue if successful
 * @retval NULL on failure
 */
struct audio_queue *audio_encoder_audio_queue_get(struct audio_encoder *encoder);

/**
 * @brief Add a channel to the audio encoder
 *
 * @param encoder Audio encoder instance to add channel to
 * @param octets_per_frame Octets per frame for the channel
 * @param stream_id Stream ID
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_encoder_add_channel(struct audio_encoder *encoder, size_t octets_per_frame,
			      uint32_t stream_id);

/**
 * @brief Start a channel
 *
 * @param encoder Audio encoder instance to start channel for
 * @param stream_id Stream ID
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_encoder_start_channel(struct audio_encoder *encoder, uint32_t stream_id);

/**
 * @brief Stop a channel
 *
 * @param encoder Audio encoder instance to stop channel for
 * @param stream_id Stream ID
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_encoder_stop_channel(struct audio_encoder *encoder, uint32_t stream_id);

/**
 * @brief Register a callback to be called on completion of each encoded frame
 *
 * @param encoder Audio encoder instance to register with
 * @param cb Callback function
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_encoder_register_cb(struct audio_encoder *encoder, audio_encoder_sdu_cb_t cb,
			      void *context);

/**
 * @brief Stop and delete an audio encoder instance
 *
 * The encoder thread is stopped, and all memory allocated by audio_encoder_create is freed.
 *
 * @param encoder The audio encoder instance to delete
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_encoder_delete(struct audio_encoder *encoder);

#endif /* _AUDIO_ENCODER_H */
