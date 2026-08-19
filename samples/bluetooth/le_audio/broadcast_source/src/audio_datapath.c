/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/audio/codec.h>
#include "bluetooth/le_audio/audio_encoder.h"
#include "bluetooth/le_audio/audio_source_i2s.h"
#include "audio_datapath.h"
#include "mic_source.h"

LOG_MODULE_REGISTER(audio_datapath, CONFIG_AUDIO_DATAPATH_LOG_LEVEL);

#if CONFIG_ALIF_BLE_AUDIO_USE_RAMFUNC
#define INT_RAMFUNC __ramfunc
#else
#define INT_RAMFUNC
#endif

#define MIC_LEVEL_CALC(_s)   (((int)(_s) * CONFIG_MICROPHONE_GAIN) / 100)
#define INPUT_LEVEL_CALC(_s) (((int)(_s) * CONFIG_INPUT_VOLUME_LEVEL) / 100)

#define I2S_NODE     DT_ALIAS(i2s_input)
#define I2S_MIC_NODE DT_ALIAS(i2s_mic)
#define CODEC_NODE   DT_ALIAS(audio_codec)

const struct device *i2s_dev = DEVICE_DT_GET(I2S_NODE);
const struct device *codec_dev = DEVICE_DT_GET(CODEC_NODE);
const struct device *i2s_mic_dev = DEVICE_DT_GET_OR_NULL(I2S_MIC_NODE);

/* Audio datapath handles */
static struct audio_encoder *audio_encoder;

#if CONFIG_APP_PRINT_STATS
static void on_frame_complete(void *param, uint32_t timestamp, uint16_t sdu_seq)
{
	if ((sdu_seq % CONFIG_APP_PRINT_STATS_INTERVAL) == 0) {
		LOG_INF("SDU sequence number: %u", sdu_seq);
	}
}
#endif

#ifdef CONFIG_PRESENTATION_COMPENSATION_DEBUG
void on_timing_debug_info_ready(struct presentation_comp_debug_data *dbg_data)
{
	LOG_INF("Presentation compensation debug data is ready");
}
#endif

static int configure_codec(const uint32_t sampling_rate_hz)
{
	int ret;
	/* clang-format off */
	struct audio_codec_cfg codec_cfg = {
		.dai_type = AUDIO_DAI_TYPE_I2S,
		.dai_cfg = {
			.i2s = {
				.word_size = AUDIO_PCM_WIDTH_16_BITS,
				.channels = 2,
				.format = I2S_FMT_DATA_FORMAT_I2S,
				.options = 0,
				.frame_clk_freq = sampling_rate_hz,
				.mem_slab = NULL,
				.block_size = 0,
				.timeout = 0,
			},
		},
	};
	/* clang-format on */

	/* Configure codec */
	ret = audio_codec_configure(codec_dev, &codec_cfg);
	if (ret) {
		LOG_ERR("Failed to configure source codec. err %d", ret);
		return ret;
	}
	audio_codec_start_output(codec_dev);

	return 0;
}

INT_RAMFUNC static void audio_encoder_mixer_thread_func(void *p1, void *p2, void *p3)
{
	/* thread:
	 * get audio input jack buffer
	 * if audio_queue_mic has data available
	 *     adjust mic gain
	 *     lower input audio gain
	 *     mix mic and audio input samples
	 *     free mic data
	 * end
	 * push audio samples to LC3 encoder
	 * free audio input buffer
	 */

	struct audio_queue *audio_queue_in1 = p1; /* input I2S codec (WM8904) */
	struct audio_queue *audio_queue_in2 = p2; /* input I2S MIC */
	struct audio_queue *audio_queue_out = p3; /* output (to LC3 encoder) */
	struct audio_block *audio_in1;
	struct audio_block *audio_in2;
	struct audio_block *audio_out;
	int ret;

	LOG_DBG("Mixer thread started");

	while (1) {
		audio_out = audio_in2 = audio_in1 = NULL;
		ret = k_msgq_get(&audio_queue_in1->msgq, &audio_in1, K_FOREVER);
		if (ret || !audio_in1) {
			continue;
		}

		ret = k_msgq_get(&audio_queue_in2->msgq, &audio_in2, K_NO_WAIT);
		if (!ret && audio_in2) {
			/* Do mixing... */
			size_t const samples =
				audio_queue_in2->audio_block_samples * audio_in2->num_channels;
			pcm_sample_t *p_mic_data = audio_in2->channels[0];
			bool const mic_has_right_channel = audio_in2->num_channels > 1;

			pcm_sample_t *p_input_left = audio_in1->channels[0];
			pcm_sample_t *p_input_right =
				audio_in1->num_channels > 1 ? audio_in1->channels[1] : NULL;
			pcm_sample_t data;

			for (size_t sample = 0; sample < samples; sample++) {
				data = *p_mic_data++;
				data = MIC_LEVEL_CALC(data);

				if ((sample & 1) && mic_has_right_channel) { /* Right channel */
					if (p_input_right) {
						*p_input_right =
							data + INPUT_LEVEL_CALC(*p_input_right);
						p_input_right++;
					}
				} else { /* Left channel */
					*p_input_left = data + INPUT_LEVEL_CALC(*p_input_left);
					p_input_left++;
				}
			}
			k_mem_slab_free(&audio_queue_in2->slab, audio_in2);
		}

		ret = k_mem_slab_alloc(&audio_queue_out->slab, (void **)&audio_out, K_NO_WAIT);
		if (ret || !audio_out) {
			k_mem_slab_free(&audio_queue_in1->slab, audio_in1);
			continue;
		}

		/* Copy in1 to out */
		memcpy(audio_out, audio_in1, sizeof(*audio_out));

		ret = k_msgq_put(&audio_queue_out->msgq, &audio_out, K_NO_WAIT);
		if (ret) {
			k_mem_slab_free(&audio_queue_out->slab, audio_out);
		}
		k_mem_slab_free(&audio_queue_in1->slab, audio_in1);
	}
}

static int configure_mic_input(void)
{
	/*
	 * Two I2S input queues are used:
	 *   - audio jack I2S input (audio_queue_i2s)
	 *   - mic I2S input (audio_queue_mic)
	 *
	 * Configured (audio encoder input) audio I2S input queue will be changed
	 * to audio_queue_i2s and the created thread will mix audio_queue_mic
	 * into audio_queue_i2s. Result will be copied and pushed to
	 * audio_queue_current (audio encoder input queue).
	 */

	struct audio_queue *audio_queue_current = audio_encoder_audio_queue_get(audio_encoder);

	if (!audio_queue_current) {
		LOG_ERR("Failed to get audio queue");
		return -ENODEV;
	}

	struct audio_queue *audio_queue_mic, *audio_queue_i2s;

	audio_queue_mic = audio_queue_create(audio_queue_current->item_count,
					     audio_queue_current->sampling_freq_hz,
					     audio_queue_current->frame_duration_us);

	if (!audio_queue_mic) {
		LOG_ERR("Failed to create audio queue");
		return -ENOMEM;
	}

	audio_queue_i2s = audio_queue_create(audio_queue_current->item_count,
					     audio_queue_current->sampling_freq_hz,
					     audio_queue_current->frame_duration_us);

	if (!audio_queue_i2s) {
		audio_queue_delete(audio_queue_mic);
		LOG_ERR("Failed to create audio queue");
		return -ENOMEM;
	}

	int ret = audio_source_i2s_configure(i2s_dev, audio_queue_i2s);

	if (ret != 0) {
		audio_queue_delete(audio_queue_i2s);
		audio_queue_delete(audio_queue_mic);
		LOG_ERR("Failed to configure audio source I2S, err %d", ret);
		return ret;
	}

	ret = mic_i2s_configure(i2s_mic_dev, audio_queue_mic);
	if (ret != 0) {
		audio_queue_delete(audio_queue_i2s);
		audio_queue_delete(audio_queue_mic);
		LOG_ERR("Failed to configure mic I2S, err %d", ret);
		return ret;
	}

	static struct k_thread mixer_thread;

	static K_THREAD_STACK_DEFINE(mixer_thread_stack, 2048);

	k_tid_t tid = k_thread_create(
		&mixer_thread, mixer_thread_stack, K_THREAD_STACK_SIZEOF(mixer_thread_stack),
		audio_encoder_mixer_thread_func, audio_queue_i2s, audio_queue_mic,
		audio_queue_current, CONFIG_ALIF_BLE_HOST_THREAD_PRIORITY + 1, 0, K_NO_WAIT);

	if (!tid) {
		audio_queue_delete(audio_queue_i2s);
		audio_queue_delete(audio_queue_mic);
		LOG_ERR("Failed to create mixer thread");
		return -EINVAL;
	}

	k_thread_name_set(tid, "mixer");

	return 0;
}

int audio_datapath_init(void)
{
	if (!device_is_ready(i2s_dev)) {
		LOG_ERR("I2S device is not ready");
		return -ENODEV;
	}
	if (!device_is_ready(codec_dev)) {
		LOG_ERR("Audio codec device is not ready");
		return -ENODEV;
	}
	if (i2s_mic_dev && !device_is_ready(i2s_mic_dev)) {
		LOG_ERR("I2S MIC device is not ready");
		return -1;
	}

	struct audio_encoder_params const enc_params = {
		.i2s_dev = i2s_dev,
		.frame_duration_us =
			IS_ENABLED(CONFIG_ALIF_BLE_AUDIO_FRAME_DURATION_10MS) ? 10000 : 7500,
		.sampling_rate_hz = CONFIG_ALIF_BLE_AUDIO_FS_HZ,
		.audio_buffer_len_us = 2 * CONFIG_ALIF_BLE_AUDIO_MAX_TLATENCY,
	};

	audio_encoder_delete(audio_encoder);

	configure_codec(enc_params.sampling_rate_hz);

	audio_encoder = audio_encoder_create(&enc_params);
	if (!audio_encoder) {
		LOG_ERR("Failed to create audio encoder");
		return -ENODEV;
	}

#if CONFIG_APP_PRINT_STATS
	int ret = audio_encoder_register_cb(audio_encoder, on_frame_complete, NULL);

	if (ret != 0) {
		LOG_ERR("Failed to register encoder cb for stats, err %d", ret);
		return ret;
	}
#endif

	if (i2s_mic_dev && configure_mic_input() != 0) {
		LOG_ERR("Failed to configure mic input");
		return -1;
	}

	return 0;
}

int audio_datapath_channel_create(const size_t octets_per_frame, const uint8_t stream_lid)
{
	int ret = audio_encoder_add_channel(audio_encoder, octets_per_frame, stream_lid);

	if (ret) {
		LOG_ERR("Channel %u creation failed. Err %d", stream_lid, ret);
		return ret;
	}
	return 0;
}

void audio_datapath_start(const uint8_t stream_lid)
{
	int retval;

	retval = audio_encoder_start_channel(audio_encoder, stream_lid);
	if (retval != 0) {
		LOG_ERR("Failed to start channel %d, err %d", stream_lid, retval);
		__ASSERT(false, "Failed to start channel");
	}
}

void audio_datapath_mic_control(bool const start)
{
	/* Do nothing if audio encoder is not initialized */
	if (!audio_encoder) {
		return;
	}

	if (start) {
		LOG_INF("MIC start...");
		mic_i2s_start();
		return;
	}

	LOG_INF("MIC stop...");
	mic_i2s_stop();
}
