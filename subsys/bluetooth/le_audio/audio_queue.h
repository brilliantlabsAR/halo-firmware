/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef _AUDIO_QUEUE_H
#define _AUDIO_QUEUE_H

#include <zephyr/kernel.h>

/* Max supported sampling rate is 48kHz.
 * 10ms frame has 480 bytes and 7.5ms has 360 bytes.
 */
#define MAX_SAMPLES_PER_AUDIO_BLOCK 480
#define MAX_NUMBER_OF_CHANNELS      2

typedef int16_t pcm_sample_t;

struct audio_block {
	uint32_t timestamp;
	/** Number of audio channels in this block */
	size_t num_channels;
	/** 16-bit signed PCM values */
	union {
		pcm_sample_t channels[MAX_NUMBER_OF_CHANNELS][MAX_SAMPLES_PER_AUDIO_BLOCK];
		struct {
			pcm_sample_t buf_left[MAX_SAMPLES_PER_AUDIO_BLOCK];
#if CONFIG_ALIF_BLE_AUDIO_NMB_CHANNELS > 1
			pcm_sample_t buf_right[MAX_SAMPLES_PER_AUDIO_BLOCK];
#endif
		};
	};
};

struct audio_queue {
	size_t item_count;
	size_t item_size;
	uint16_t audio_block_samples;
	uint16_t frame_duration_us;
	size_t sampling_freq_hz;
	struct k_mem_slab slab;
	struct k_msgq msgq;
	uint8_t buf[];
};

/**
 * @brief Dynamically allocate and initialise an audio queue
 *
 * For most use cases dynamic allocation is required since the size of each audio block is not known
 * until the parameters of the stream are agreed between this device and the peer device (broadcast
 * source use case is an exception where all parameters can be fixed at compile time).
 *
 * @param item_count Number of audio blocks in the queue
 * @param sampling_freq_hz Sampling frequency in Hz
 * @param frame_duration_us Frame duration. @ref enum audio_queue_duration
 *
 * @retval Pointer to created audio queue header if successful
 * @retval NULL if an error occurred
 */
struct audio_queue *audio_queue_create(size_t item_count, size_t sampling_freq_hz,
				       size_t frame_duration_us);

/**
 * @brief Delete an audio queue that was previously dynamically allocated
 *
 * @param queue Pointer to the queue to delete
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int audio_queue_delete(struct audio_queue *queue);

#endif /* _AUDIO_QUEUE_H */
