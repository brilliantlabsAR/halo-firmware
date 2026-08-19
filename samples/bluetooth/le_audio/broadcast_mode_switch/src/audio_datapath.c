/* Copyright Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/audio/codec.h>
#include <zephyr/random/random.h>
#include "bluetooth/le_audio/audio_decoder.h"
#include "bluetooth/le_audio/audio_encoder.h"
#include "bluetooth/le_audio/audio_source_i2s.h"
#include "bluetooth/le_audio/audio_utils.h"
#include "bluetooth/le_audio/presentation_compensation.h"
#include "audio_datapath.h"

LOG_MODULE_REGISTER(audio_datapath, CONFIG_BLE_AUDIO_LOG_LEVEL);

#define CODEC_NODE DT_ALIAS(audio_codec)
#define I2S_NODE   DT_ALIAS(i2s_bus)

struct audio_datapath {
	struct audio_decoder *decoder;
	struct audio_encoder *encoder;
	size_t octets_per_frame;
};

static struct audio_datapath env;

static int broadcast_audio_path_init(void)
{
	int ret;

	/* Check all devices are ready */
	ret = device_is_ready(DEVICE_DT_GET(I2S_NODE));

	if (!ret) {
		LOG_ERR("I2S is not ready");
		return -1;
	}

	ret = device_is_ready(DEVICE_DT_GET(CODEC_NODE));

	if (!ret) {
		LOG_ERR("Audio codec is not ready");
		return -1;
	}

	return 0;
}

SYS_INIT(broadcast_audio_path_init, APPLICATION, 0);

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

int audio_datapath_start_sink(void)
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

	LOG_INF("Audio decoder deleted");

	return 0;
}

#if CONFIG_APP_PRINT_STATS
static void on_frame_complete(void *param, uint32_t timestamp, uint16_t sdu_seq)
{
	if ((sdu_seq % CONFIG_APP_PRINT_STATS_INTERVAL) == 0) {
		LOG_INF("SDU sequence number: %u", sdu_seq);
	}
}
#endif

int audio_datapath_create_source(struct audio_datapath_config const *const cfg)
{
	int ret;

	if (!cfg) {
		__ASSERT(false, "Datapath configuration missing");
		return -EINVAL;
	}

	if (env.encoder) {
		return -EALREADY;
	}

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
		LOG_ERR("Failed to configure source codec. err %d", ret);
		return ret;
	}
	audio_codec_start_output(DEVICE_DT_GET(CODEC_NODE));

	struct audio_encoder_params const enc_params = {
		.i2s_dev = cfg->i2s_dev,
		.frame_duration_us = cfg->frame_duration_is_10ms ? 10000 : 7500,
		.sampling_rate_hz = cfg->sampling_rate_hz,
		.audio_buffer_len_us = cfg->pres_delay_us,
	};

	env.encoder = audio_encoder_create(&enc_params);
	if (!env.encoder) {
		LOG_ERR("Failed to create audio encoder");
		return -ENODEV;
	}

#if CONFIG_APP_PRINT_STATS
	int ret = audio_encoder_register_cb(env.encoder, on_frame_complete, NULL);

	if (ret != 0) {
		LOG_ERR("Failed to register encoder cb for stats, err %d", ret);
		return ret;
	}
#endif

	LOG_INF("Source audio datapath initialized");
	return 0;
}

int audio_datapath_channel_create_source(size_t octets_per_frame, uint8_t stream_lid)
{
	if (!env.encoder) {
		return -EINVAL;
	}

	int err = audio_encoder_add_channel(env.encoder, octets_per_frame, stream_lid);

	if (err) {
		LOG_ERR("Failed to add stream channel %u. err %d", stream_lid, err);
		return err;
	}

	return 0;
}

int audio_datapath_channel_start_source(uint8_t stream_lid)
{
	if (!env.encoder) {
		return -EINVAL;
	}

	int retval = audio_encoder_start_channel(env.encoder, stream_lid);

	if (retval != 0) {
		LOG_ERR("Failed to start channel %d, err %d", stream_lid, retval);
		__ASSERT(false, "Failed to start channel");
	}

	LOG_INF("Source audio datapath started for stream %d", stream_lid);
	return 0;
}

int audio_datapath_cleanup_source(void)
{
	if (env.encoder) {
		audio_encoder_delete(env.encoder);
		env.encoder = NULL;

		audio_codec_stop_output(DEVICE_DT_GET(CODEC_NODE));

		LOG_INF("Audio encoder deleted");
	}

	return 0;
}
