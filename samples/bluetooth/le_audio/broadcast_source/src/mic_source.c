/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/util.h>
#include "drivers/i2s_sync.h"
#include "gapi_isooshm.h"
#include "bluetooth/le_audio/audio_queue.h"
#include "mic_source.h"

struct mic_source_env {
	const struct device *dev;
	struct audio_queue *audio_queue;
	size_t block_size;
	size_t number_of_channels;
	bool started;
	bool capture;
};

static struct mic_source_env mic_source;

#if CONFIG_ALIF_BLE_AUDIO_USE_RAMFUNC
#define INT_RAMFUNC __ramfunc
#else
#define INT_RAMFUNC
#endif

INT_RAMFUNC static void recv_next_block(const struct device *dev)
{
	struct audio_block *p_block = NULL;
	int ret = k_mem_slab_alloc(&mic_source.audio_queue->slab, (void **)&p_block, K_NO_WAIT);

	if (ret || !p_block) {
		return;
	}

	i2s_sync_recv(dev, p_block->channels, mic_source.block_size);

	p_block->timestamp = 0;
	p_block->num_channels = mic_source.number_of_channels;
}

INT_RAMFUNC static void on_data_received(const struct device *dev,
					 const enum i2s_sync_status status, void *block)
{
	ARG_UNUSED(status);

	recv_next_block(dev);

	if (!block) {
		return;
	}

	struct audio_block *p_block = CONTAINER_OF(block, struct audio_block, channels);

	if (!mic_source.capture) {
		/* Just ignore the block */
		k_mem_slab_free(&mic_source.audio_queue->slab, p_block);
		return;
	}

	if (k_msgq_put(&mic_source.audio_queue->msgq, &p_block, K_NO_WAIT)) {
		/* Failed to put into queue */
		k_mem_slab_free(&mic_source.audio_queue->slab, p_block);
	}
}

int mic_i2s_configure(const struct device *dev, struct audio_queue *audio_queue)
{
	if (!dev || !audio_queue) {
		return -EINVAL;
	}

	struct i2s_sync_config i2s_cfg;

	if (i2s_sync_get_config(dev, &i2s_cfg)) {
		return -EIO;
	}

	if (i2s_cfg.channel_count > MAX_NUMBER_OF_CHANNELS) {
		return -EINVAL;
	}

	i2s_cfg.sample_rate = audio_queue->sampling_freq_hz;
	if (i2s_sync_configure(dev, &i2s_cfg)) {
		return -EIO;
	}

	/* Shutdown existing stream and wait for start */
	i2s_sync_disable(dev, I2S_DIR_RX);

	mic_source.dev = dev;
	mic_source.audio_queue = audio_queue;
	mic_source.block_size =
		i2s_cfg.channel_count * audio_queue->audio_block_samples * sizeof(pcm_sample_t);
	mic_source.number_of_channels = i2s_cfg.channel_count;
	mic_source.started = false;
	mic_source.capture = false;

	int ret = i2s_sync_register_cb(dev, I2S_DIR_RX, on_data_received);

	if (ret) {
		return ret;
	}

	return 0;
}

void mic_i2s_start(void)
{
	if (!mic_source.dev) {
		return;
	}

	/* Kick the I2S receive operation */
	if (!mic_source.started) {
		recv_next_block(mic_source.dev);
		mic_source.started = true;
	}

	mic_source.capture = true;
}

void mic_i2s_stop(void)
{
	if (!mic_source.dev) {
		return;
	}

	if (!mic_source.started) {
		return;
	}

	mic_source.capture = false;
}
