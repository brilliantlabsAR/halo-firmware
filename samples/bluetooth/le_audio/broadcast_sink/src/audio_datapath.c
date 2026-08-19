/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/logging/log.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/audio/codec.h>
#include "bluetooth/le_audio/audio_decoder.h"
#include "bluetooth/le_audio/presentation_compensation.h"
#include "audio_datapath.h"

LOG_MODULE_REGISTER(audio_datapath, CONFIG_BLE_AUDIO_LOG_LEVEL);

#define CODEC_NODE DT_ALIAS(audio_codec)

struct audio_datapath {
	struct audio_decoder *decoder;
	size_t octets_per_frame;
};

static struct audio_datapath env;

#if CONFIG_APP_PRINT_STATS
static void print_sdus(void *context, uint32_t timestamp, uint16_t sdu_seq)
{
	static uint16_t last_sdu;

	if (last_sdu && (last_sdu + 1 < sdu_seq)) {
		LOG_INF("SDU sequence number jumps, last: %u, now: %u", last_sdu, sdu_seq);
	}
	last_sdu = sdu_seq;

	if (0 == (sdu_seq % 128)) {
		LOG_INF("SDU sequence number %u", sdu_seq);
	}
}
#endif

#ifdef CONFIG_PRESENTATION_COMPENSATION_DEBUG
void on_timing_debug_info_ready(struct presentation_comp_debug_data *dbg_data)
{
	LOG_INF("Presentation compensation debug data is ready");
}
#endif

int audio_datapath_create_sink(struct audio_datapath_config const *const cfg)
{
	int ret;

	if (!cfg) {
		__ASSERT(false, "Datapath configuration missing");
		return -EINVAL;
	}

	if (env.decoder) {
		return -EALREADY;
	}

	env.octets_per_frame = cfg->octets_per_frame;

	/* Configure codec */
	struct audio_codec_cfg codec_cfg = {
		.dai_type = AUDIO_DAI_TYPE_I2S,
		.dai_cfg = {
			.i2s = {
				.word_size = AUDIO_PCM_WIDTH_16_BITS,
				.channels = 2,
				.format = I2S_FMT_DATA_FORMAT_I2S,
				.options = 0,
				.frame_clk_freq = cfg->sampling_rate_hz,
				.mem_slab = NULL,
				.block_size = 0,
				.timeout = 0,
			},
		},
	};

	ret = audio_codec_configure(DEVICE_DT_GET(CODEC_NODE), &codec_cfg);
	if (ret) {
		LOG_ERR("Failed to configure sink codec. err %d", ret);
		return ret;
	}
	audio_codec_start_output(DEVICE_DT_GET(CODEC_NODE));

	struct audio_decoder_params const dec_params = {
		.i2s_dev = cfg->i2s_dev,
		.frame_duration_us = cfg->frame_duration_is_10ms ? 10000 : 7500,
		.sampling_rate_hz = cfg->sampling_rate_hz,
		.pres_delay_us = cfg->pres_delay_us,
	};

	env.decoder = audio_decoder_create(&dec_params);
	if (env.decoder == NULL) {
		LOG_ERR("Failed to create audio decoder");
		return -ENOMEM;
	}

	/* TODO: Change MCLK to use alif clock control */

	/* ret = presentation_compensation_configure(cfg->mclk_dev, pres_delay_us);
	 * if (ret != 0) {
	 *	LOG_ERR("Failed to configure presentation compensation module, err %d", ret);
	 *	return ret;
	 * }
	 */

	/* Add presentation compensation callback to notify audio sink */
	/* ret = presentation_compensation_register_cb(audio_sink_i2s_apply_timing_correction);
	 * if (ret != 0) {
	 *	LOG_ERR("Failed to register presentation compensation callback, err %d", ret);
	 *	return ret;
	 * }
	 */

#ifdef CONFIG_PRESENTATION_COMPENSATION_DEBUG
	/* ret = presentation_compensation_register_debug_cb(on_timing_debug_info_ready);
	 * if (ret != 0) {
	 *	LOG_ERR("Failed to register presentation compensation debug callback, err %d", ret);
	 *	return ret;
	 * }
	 */
#endif

#if CONFIG_APP_PRINT_STATS
	ret = audio_decoder_register_cb(env.decoder, print_sdus, NULL);

	if (ret != 0) {
		LOG_ERR("Failed to register decoder cb, err %d", ret);
		return ret;
	}
#endif

	LOG_INF("Created audio datapath");

	return 0;
}

int audio_datapath_channel_create_sink(size_t const octets_per_frame, uint8_t const ch_index)
{
	int ret = audio_decoder_add_channel(env.decoder, octets_per_frame, ch_index);

	if (ret) {
		LOG_ERR("Failed to create channel %u, err %d", ch_index, ret);
		return ret;
	}

	return 0;
}

int audio_datapath_channel_start_sink(uint8_t const ch_index)
{
	int ret = audio_decoder_start_channel(env.decoder, ch_index);

	if (ret) {
		LOG_ERR("Failed to start channel %u, err %d", ch_index, ret);
		return ret;
	}

	return 0;
}

int audio_datapath_start(void)
{
	for (int iter = 0; iter < CONFIG_ALIF_BLE_AUDIO_NMB_CHANNELS; iter++) {
		audio_datapath_channel_create_sink(env.octets_per_frame, iter);
	}

	for (int iter = 0; iter < CONFIG_ALIF_BLE_AUDIO_NMB_CHANNELS; iter++) {
		audio_datapath_channel_start_sink(iter);
	}

	LOG_INF("Audio datapath started");

	return 0;
}

int audio_datapath_cleanup_sink(void)
{
	audio_decoder_delete(env.decoder);
	env.decoder = NULL;

	/* Stop codec output */
	audio_codec_stop_output(DEVICE_DT_GET(CODEC_NODE));

	LOG_INF("Removed audio datapath");

	return 0;
}
