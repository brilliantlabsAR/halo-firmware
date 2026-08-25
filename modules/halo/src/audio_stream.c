/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <halo/audio_stream.h>
#include <halo/mem_manager.h>
#include <halo/file_manager.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/init.h>
#include <t5838.h>
#include "alif_lc3.h"

#if defined(CONFIG_HALO_AUDIO_HPF)
#include <halo/audio_eq.h>
#endif

#if defined(CONFIG_MAX98357A_AUDIO)
#include <max98357a_audio.h>
#endif

LOG_MODULE_REGISTER(audio_stream, CONFIG_HALO_LOG_LEVEL);

static const char *audio_owner_name(audio_owner_t owner)
{
	switch (owner) {
	case AUDIO_OWNER_NONE:
		return "None";
	case AUDIO_OWNER_LUA:
		return "Lua";
	case AUDIO_OWNER_SYSTEM:
		return "System";
	case AUDIO_OWNER_LE_AUDIO:
		return "LE Audio";
	default:
		return "Unknown";
	}
}

/* ============================================================================
 * LC3 Codec Implementation
 * ============================================================================ */

static int lc3_codec_init(void)
{
	int ret = alif_lc3_init();
	if (ret != 0) {
		LOG_ERR("Failed to initialize LC3 codec: %d", ret);
		return ret;
	}
	return 0;
}

SYS_INIT(lc3_codec_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* Bad-frame scoring for garbage-input muting.
 *
 * PLC is meant to paper over the occasional lost frame. A continuous stream
 * of bad frames (wrong framing, non-LC3 data fed to the decoder) makes PLC
 * extrapolate full-scale broadband garbage indefinitely - observed on
 * hardware to draw enough current to trip the battery protection IC even
 * with the speaker protection chain engaged.
 *
 * A simple consecutive-bad-frame counter is not sufficient: random data
 * frequently parses as a "valid" LC3 frame - observed on hardware at up to
 * ~3 in 4 frames - which resets the counter, keeps the score drained, and
 * lets garbage play (or oscillate in and out of mute; a battery trip was
 * reproduced during exactly such an unmuted window). Two mechanisms:
 *
 *  - ENGAGE on a score that rises fast (+4 per bad frame) and drains
 *    slowly (-1 per good frame): even 25 %-bad mixed garbage accumulates
 *    and mutes within ~300 ms; all-bad input mutes in 2 frames (20 ms).
 *  - RELEASE only after LC3_UNMUTE_GOOD_FRAMES *consecutive* good frames
 *    (any bad frame resets the run), so a garbage storm cannot oscillate
 *    the mute. While muted, even valid-parsing frames are silenced - a
 *    good-parsing frame inside a garbage storm is still garbage.
 *
 * This is free for legitimate playback: LC3 frames arrive over reliable
 * GATT writes, so there is no radio frame loss - a bad frame can only mean
 * corrupt data. A lone corrupt frame gets PLC as before (score +4 never
 * reaches the mute threshold) and drains away in 4 clean frames.
 */
#define LC3_SCORE_BAD           4   /* added per bad frame */
#define LC3_SCORE_GOOD          1   /* drained per good frame */
#define LC3_SCORE_MAX           12
#define LC3_MUTE_SCORE          8   /* mute at/above this score */
#define LC3_UNMUTE_GOOD_FRAMES  50  /* consecutive good frames to unmute (~500 ms) */

struct audio_codec_ctx {
	lc3_cfg_t config;
	union {
		lc3_encoder_t encoder;
		lc3_decoder_t decoder;
	} lc3;
	int32_t *scratch;
	int32_t *status;
	int sample_rate;
	int duration;
	int bitrate;
	bool is_encoder;
	int bad_score;    /**< garbage-input score (decoder only) */
	int good_streak;  /**< consecutive good frames while muted */
	bool muted;       /**< output muted due to sustained bad input */
};

audio_codec_ctx_t *audio_lc3_decoder_create(int sample_rate, int duration, int bitrate)
{
	audio_codec_ctx_t *ctx = halo_malloc(sizeof(*ctx), HALO_MEM_REGION_AUTO);
	if (!ctx) {
		LOG_ERR("Failed to allocate decoder context");
		return NULL;
	}

	memset(ctx, 0, sizeof(*ctx));
	ctx->sample_rate = sample_rate;
	ctx->duration = duration;
	ctx->bitrate = bitrate;
	ctx->is_encoder = false;

	/* Configure LC3 */
	lc3_api_configure(&ctx->config, sample_rate, duration);

	/* Allocate scratch buffer (use internal memory for performance) */
	ctx->scratch =
		halo_malloc(lc3_api_decoder_scratch_size(&ctx->config), HALO_MEM_REGION_AUTO);
	if (!ctx->scratch) {
		LOG_ERR("Failed to allocate decoder scratch buffer");
		halo_free(ctx);
		return NULL;
	}

	/* Initialize decoder */
	ctx->status =
		halo_malloc(lc3_api_decoder_status_size(&ctx->config), HALO_MEM_REGION_AUTO);
	if (!ctx->status) {
		LOG_ERR("Failed to allocate decoder status buffer");
		halo_free(ctx->scratch);
		halo_free(ctx);
		return NULL;
	}

	lc3_api_initialise_decoder(&ctx->config, &ctx->lc3.decoder, ctx->status);

	return ctx;
}

int audio_lc3_decode_frame(audio_codec_ctx_t *ctx, const uint8_t *encoded_data, size_t encoded_len,
			   int16_t *pcm_out, size_t pcm_len)
{
	if (!ctx || ctx->is_encoder || !encoded_data || !pcm_out) {
		return -EINVAL;
	}

	uint8_t bec_detect = 0;
	int ret = lc3_api_decode_frame(&ctx->config, &ctx->lc3.decoder, encoded_data, encoded_len,
				       0, &bec_detect, pcm_out, ctx->scratch);

	if (ret != 0) {
		LOG_ERR("LC3 decode failed: %d", ret);
		return -EIO;
	}

	if (bec_detect) {
		ctx->good_streak = 0;
		ctx->bad_score += LC3_SCORE_BAD;
		if (ctx->bad_score > LC3_SCORE_MAX) {
			ctx->bad_score = LC3_SCORE_MAX;
		}
		if (!ctx->muted && ctx->bad_score >= LC3_MUTE_SCORE) {
			ctx->muted = true;
			LOG_WRN("Sustained bad LC3 input - muting");
		} else if (!ctx->muted) {
			LOG_WRN("Bad LC3 frame detected, PLC applied");
		}
	} else {
		if (ctx->bad_score > 0) {
			ctx->bad_score -= LC3_SCORE_GOOD;
		}
		if (ctx->muted &&
		    ++ctx->good_streak >= LC3_UNMUTE_GOOD_FRAMES) {
			ctx->muted = false;
			ctx->good_streak = 0;
			ctx->bad_score = 0;
			LOG_INF("LC3 input recovered - unmuting");
		}
	}

	if (ctx->muted) {
		/* See comment at LC3_MUTE_SCORE: while muted, even
		 * valid-parsing frames are silenced.
		 */
		memset(pcm_out, 0, pcm_len); /* pcm_len is bytes */
	}

	return 0;
}

void audio_lc3_decoder_destroy(audio_codec_ctx_t *ctx)
{
	if (ctx) {
		halo_free(ctx->scratch);
		halo_free(ctx->status);
		halo_free(ctx);
	}
}

audio_codec_ctx_t *audio_lc3_encoder_create(int sample_rate, int duration, int bitrate)
{
	audio_codec_ctx_t *ctx = halo_malloc(sizeof(*ctx), HALO_MEM_REGION_AUTO);
	if (!ctx) {
		LOG_ERR("Failed to allocate encoder context");
		return NULL;
	}

	memset(ctx, 0, sizeof(*ctx));
	ctx->sample_rate = sample_rate;
	ctx->duration = duration;
	ctx->bitrate = bitrate;
	ctx->is_encoder = true;

	/* Configure LC3 */
	lc3_api_configure(&ctx->config, sample_rate, duration);

	/* Allocate scratch buffer */
	ctx->scratch =
		halo_malloc(lc3_api_encoder_scratch_size(&ctx->config), HALO_MEM_REGION_AUTO);
	if (!ctx->scratch) {
		LOG_ERR("Failed to allocate encoder scratch buffer");
		halo_free(ctx);
		return NULL;
	}

	/* Initialize encoder */
	lc3_api_initialise_encoder(&ctx->config, &ctx->lc3.encoder);

	return ctx;
}

int audio_lc3_encode_frame(audio_codec_ctx_t *ctx, const int16_t *pcm_data, size_t pcm_len,
			   uint8_t *encoded_out, size_t encoded_len)
{
	if (!ctx || !ctx->is_encoder || !pcm_data || !encoded_out) {
		return -EINVAL;
	}

	/* Note: LC3 API doesn't use const for input, but won't modify it */
	int ret = lc3_api_encode_frame(&ctx->config, &ctx->lc3.encoder, (int16_t *)pcm_data,
				       encoded_out, encoded_len, ctx->scratch);

	if (ret != 0) {
		LOG_ERR("LC3 encode failed: %d", ret);
		return -EIO;
	}

	return 0;
}

void audio_lc3_encoder_destroy(audio_codec_ctx_t *ctx)
{
	if (ctx) {
		halo_free(ctx->scratch);
		halo_free(ctx);
	}
}

size_t audio_lc3_get_frame_size(int sample_rate, int duration, int bitrate)
{
	return lc3_api_get_byte_count(bitrate, sample_rate, duration);
}

size_t audio_lc3_get_pcm_frame_size(int sample_rate, int duration)
{
	/* duration is µs/10 to match the Alif LC3 enum (750 = 7.5 ms, 1000 = 10 ms),
	 * so PCM frame size in bytes = (sample_rate * duration / 100000) * sizeof(int16_t) */
	size_t samples = (sample_rate * duration) / 100000;
	return samples * sizeof(int16_t);
}

/* ============================================================================
 * Speaker Hardware Implementation
 * ============================================================================ */

struct audio_speaker {
	const struct device *dev;
	struct audio_config config;
	bool is_started;
	int volume;  /* Current volume level (0-100) */
};

/* Speaker singleton management */
K_MUTEX_DEFINE(speaker_lock);

#if defined(CONFIG_HALO_AUDIO_HPF)
static struct audio_hpf speaker_hpf;
#endif

static struct {
	struct audio_speaker speaker;  /* Static speaker instance */
	audio_owner_t owner;           /* Current owner */
	bool is_initialized;           /* Initialization state */
} speaker_singleton = {
	.owner = AUDIO_OWNER_NONE,
	.is_initialized = false,
};

audio_speaker_t *audio_speaker_init(int sample_rate, int bit_depth, int channels,
				    audio_owner_t owner)
{
	k_mutex_lock(&speaker_lock, K_FOREVER);

	/* Check if already initialized and owned */
	if (speaker_singleton.is_initialized) {
		/* If owned by LE Audio, reject all non-LE Audio requests */
		if (speaker_singleton.owner == AUDIO_OWNER_LE_AUDIO &&
		    owner != AUDIO_OWNER_LE_AUDIO) {
			LOG_ERR("Speaker occupied by LE Audio");
			k_mutex_unlock(&speaker_lock);
			return NULL;
		}

		/* LE Audio can preempt any other owner */
		if (owner == AUDIO_OWNER_LE_AUDIO) {
			LOG_WRN("LE Audio preempting speaker from %s",
				audio_owner_name(speaker_singleton.owner));
			
			/* Stop current playback */
			audio_speaker_stop(&speaker_singleton.speaker);
			
			/* Mark as uninitialized for re-initialization */
			speaker_singleton.is_initialized = false;
		} else {
			/* Non-LE Audio trying to use already occupied resource */
			LOG_ERR("Speaker already occupied");
			k_mutex_unlock(&speaker_lock);
			return NULL;
		}
	}

	/* Initialize speaker using static instance */
	memset(&speaker_singleton.speaker, 0, sizeof(speaker_singleton.speaker));

	/* Get speaker device */
	speaker_singleton.speaker.dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_speaker));
	if (!device_is_ready(speaker_singleton.speaker.dev)) {
		LOG_ERR("Speaker device not ready");
		k_mutex_unlock(&speaker_lock);
		return NULL;
	}

#if defined(CONFIG_MAX98357A_AUDIO)
	/* Per-stream pre-gain is transient: every acquisition starts at
	 * unity so one owner's gain cannot leak into the next stream.
	 */
	max98357a_audio_set_stream_pregain(0);
#endif

	/* Configure audio parameters */
	speaker_singleton.speaker.config.sample_rate = sample_rate;
	speaker_singleton.speaker.config.bits_per_sample = bit_depth;
	speaker_singleton.speaker.config.channels = (channels == 1) ? AUDIO_MONO : AUDIO_STEREO;
	speaker_singleton.speaker.config.is_signed = true;

	int ret = audio_configure(speaker_singleton.speaker.dev, &speaker_singleton.speaker.config);
	if (ret != 0) {
		LOG_ERR("Failed to configure speaker: %d", ret);
		k_mutex_unlock(&speaker_lock);
		return NULL;
	}

	speaker_singleton.speaker.is_started = false;

	/* Load and apply saved volume setting */
	int saved_volume = 50; /* default */
	if (halo_settings_get("audio/volume", &saved_volume, sizeof(saved_volume)) == 0) {
		audio_speaker_set_volume(&speaker_singleton.speaker, saved_volume);
	} else {
		/* Set default volume if not saved */
		audio_speaker_set_volume(&speaker_singleton.speaker, saved_volume);
	}

	speaker_singleton.is_initialized = true;
	speaker_singleton.owner = owner;

#if defined(CONFIG_HALO_AUDIO_HPF)
	{
		int hpf_channels = (channels >= 2) ? 2 : 1;
		int ret_hpf = audio_hpf_init(&speaker_hpf, sample_rate,
					     CONFIG_HALO_AUDIO_HPF_CUTOFF_HZ,
					     hpf_channels,
					     CONFIG_HALO_AUDIO_HPF_ORDER);
		if (ret_hpf != 0) {
			LOG_WRN("Failed to init HPF: %d (audio will play unfiltered)", ret_hpf);
		}
	}
#endif

	LOG_DBG("Speaker acquired by %s", audio_owner_name(owner));

	k_mutex_unlock(&speaker_lock);
	return &speaker_singleton.speaker;
}

int audio_speaker_start(audio_speaker_t *spk)
{
	if (!spk) {
		return -EINVAL;
	}

	int ret = audio_trigger(spk->dev, AUDIO_TRIGGER_START);
	if (ret != 0) {
		LOG_ERR("Failed to start speaker: %d", ret);
		return ret;
	}

#if defined(CONFIG_HALO_AUDIO_HPF)
	/* Reset filter state to avoid transient click on stream start */
	audio_hpf_reset(&speaker_hpf);
#endif

	spk->is_started = true;
	return 0;
}

int audio_speaker_write(audio_speaker_t *spk, uint8_t *data, size_t len)
{
	if (!spk || !data) {
		return -EINVAL;
	}

	if (!spk->is_started) {
		return -ENODEV;
	}

#if defined(CONFIG_HALO_AUDIO_HPF)
	/* Apply high-pass filter to protect bone conduction speaker */
	if (speaker_hpf.enabled && len >= sizeof(int16_t)) {
		audio_hpf_process(&speaker_hpf, (int16_t *)data,
				  len / sizeof(int16_t));
	}
#endif

	size_t offset = 0;

	/* Write data in chunks, handling timeouts */
	while (offset < len && spk->is_started) {
		size_t bytes_written = 0;
		size_t remaining = len - offset;

		int ret = audio_write(spk->dev, data + offset, remaining, &bytes_written,
				      K_MSEC(1000));

		if (ret && ret != -ETIMEDOUT) {
			return ret;
		}

		offset += bytes_written;
	}

	return offset;
}

static int audio_speaker_set_volume_internal(audio_speaker_t *spk, int volume, bool persist)
{
	if (!spk) {
		return -EINVAL;
	}

	if (volume < 0 || volume > 100) {
		LOG_ERR("Invalid volume: %d (must be 0-100)", volume);
		return -EINVAL;
	}

	int ret = audio_set_volume(spk->dev, volume);
	if (ret != 0) {
		LOG_ERR("Failed to set volume: %d", ret);
		return ret;
	}

	/* Store current volume in structure */
	spk->volume = volume;

	/* Save volume setting persistently */
	if (persist) {
		halo_settings_set("audio/volume", &volume, sizeof(volume));
	}

	/* Volume changes are user-driven; skip verbose logging */
	return 0;
}

int audio_speaker_set_volume(audio_speaker_t *spk, int volume)
{
	return audio_speaker_set_volume_internal(spk, volume, true);
}

int audio_speaker_set_volume_transient(audio_speaker_t *spk, int volume)
{
	return audio_speaker_set_volume_internal(spk, volume, false);
}

void audio_speaker_notify_display_active(bool active)
{
#if defined(CONFIG_MAX98357A_AUDIO)
	max98357a_audio_set_display_active(active);
#else
	ARG_UNUSED(active);
#endif
}

int audio_speaker_set_stream_gain(audio_speaker_t *spk, int gain_db10)
{
	if (!spk) {
		return -EINVAL;
	}
	if (gain_db10 < 0 || gain_db10 > 120) {
		return -EINVAL;
	}

#if defined(CONFIG_MAX98357A_AUDIO)
	max98357a_audio_set_stream_pregain(gain_db10);
	return 0;
#else
	return (gain_db10 == 0) ? 0 : -ENOTSUP;
#endif
}

int audio_speaker_get_volume(audio_speaker_t *spk)
{
	if (spk) {
		return spk->volume;
	} else {
		/* If no speaker provided, get from settings */
		int volume = 50; /* default */
		if (halo_settings_get("audio/volume", &volume, sizeof(volume)) != 0) {
			volume = 50;
		}
		return volume;
	}
}

int audio_speaker_stop(audio_speaker_t *spk)
{
	if (!spk) {
		return -EINVAL;
	}

	if (!spk->is_started) {
		return 0; /* Already stopped */
	}

	int ret = audio_trigger(spk->dev, AUDIO_TRIGGER_STOP);
	if (ret != 0) {
		LOG_ERR("Failed to stop speaker: %d", ret);
		return ret;
	}

	spk->is_started = false;
	return 0;
}

void audio_speaker_destroy(audio_speaker_t *spk)
{
	if (!spk) {
		return;
	}

	k_mutex_lock(&speaker_lock, K_FOREVER);

	/* Only destroy if this is the current singleton instance */
	if (spk == &speaker_singleton.speaker && speaker_singleton.is_initialized) {
		audio_speaker_stop(spk);

		LOG_DBG("Speaker released by %s", audio_owner_name(speaker_singleton.owner));

		speaker_singleton.is_initialized = false;
		speaker_singleton.owner = AUDIO_OWNER_NONE;
	}

	k_mutex_unlock(&speaker_lock);
}

bool audio_speaker_check_owner(audio_speaker_t *spk, audio_owner_t expected_owner)
{
	if (!spk) {
		return false;
	}

	k_mutex_lock(&speaker_lock, K_FOREVER);
	bool result = (speaker_singleton.is_initialized &&
		       speaker_singleton.owner == expected_owner);
	k_mutex_unlock(&speaker_lock);

	return result;
}

/* ============================================================================
 * Microphone Hardware Implementation
 * ============================================================================ */

#define MIC_BLOCK_INTERVAL_MS 20
#define MIC_MEM_BLOCK_COUNT   40
/* Cap total slab memory so high sample rates (32/48 kHz stereo for LE Audio)
 * don't balloon the allocation; consumers drain blocks every 20 ms anyway. */
#define MIC_MEM_SLAB_MAX_BYTES (64 * 1024)
#define MIC_MEM_BLOCK_COUNT_MIN 8

static uint32_t mic_hw_start_ms;
static bool mic_first_dmic_read_logged;

/* Calculate block size based on configuration */
#define MIC_BLOCK_SIZE_CALC(rate, bits, ch, ms) ((rate) * (bits) * (ch) / 8 * (ms) / 1000)

struct audio_microphone {
	const struct device *dmic;
	struct dmic_cfg config;
	struct k_mem_slab slab;
	uint8_t *slab_buffer;
	size_t block_size;
	size_t block_count;
	bool is_started;
	int gain;  /* Current gain level (0-100) */
};

/* Microphone singleton management */
K_MUTEX_DEFINE(mic_lock);

static struct {
	struct audio_microphone microphone;  /* Static microphone instance */
	audio_owner_t owner;                 /* Current owner */
	bool is_initialized;                 /* Initialization state */
} mic_singleton = {
	.owner = AUDIO_OWNER_NONE,
	.is_initialized = false,
};

audio_microphone_t *audio_microphone_init(int sample_rate, int bit_depth, int channels, int gain,
					  audio_owner_t owner)
{
	k_mutex_lock(&mic_lock, K_FOREVER);

	/* Check if already initialized and owned */
	if (mic_singleton.is_initialized) {
		/* If owned by LE Audio, reject all non-LE Audio requests */
		if (mic_singleton.owner == AUDIO_OWNER_LE_AUDIO &&
		    owner != AUDIO_OWNER_LE_AUDIO) {
			LOG_ERR("Microphone occupied by LE Audio");
			k_mutex_unlock(&mic_lock);
			return NULL;
		}

		/* LE Audio can preempt any other owner */
		if (owner == AUDIO_OWNER_LE_AUDIO) {
			LOG_WRN("LE Audio preempting microphone from %s",
				audio_owner_name(mic_singleton.owner));
			
			/* Stop current capture and free resources */
			audio_microphone_stop(&mic_singleton.microphone);
			if (mic_singleton.microphone.slab_buffer) {
				halo_free(mic_singleton.microphone.slab_buffer);
				mic_singleton.microphone.slab_buffer = NULL;
			}
			
			/* Mark as uninitialized for re-initialization */
			mic_singleton.is_initialized = false;
		} else {
			/* Non-LE Audio trying to use already occupied resource */
			LOG_ERR("Microphone already occupied");
			k_mutex_unlock(&mic_lock);
			return NULL;
		}
	}

	/* Initialize microphone using static instance */
	memset(&mic_singleton.microphone, 0, sizeof(mic_singleton.microphone));

	/* Get DMIC device */
	mic_singleton.microphone.dmic = DEVICE_DT_GET(DT_CHOSEN(zephyr_micphone));
	if (!device_is_ready(mic_singleton.microphone.dmic)) {
		LOG_ERR("DMIC device not ready");
		k_mutex_unlock(&mic_lock);
		return NULL;
	}

	/* Calculate block size */
	mic_singleton.microphone.block_size =
		MIC_BLOCK_SIZE_CALC(sample_rate, bit_depth, channels, MIC_BLOCK_INTERVAL_MS);

	/* Scale block count down at high rates to keep total slab memory bounded */
	mic_singleton.microphone.block_count = MIC_MEM_BLOCK_COUNT;
	if (mic_singleton.microphone.block_size * mic_singleton.microphone.block_count >
	    MIC_MEM_SLAB_MAX_BYTES) {
		mic_singleton.microphone.block_count =
			MAX(MIC_MEM_BLOCK_COUNT_MIN,
			    MIC_MEM_SLAB_MAX_BYTES / mic_singleton.microphone.block_size);
	}

	/* Allocate memory slab for DMIC buffers */
	mic_singleton.microphone.slab_buffer =
		halo_malloc(mic_singleton.microphone.block_size *
				    mic_singleton.microphone.block_count,
			    HALO_MEM_REGION_AUTO);
	if (!mic_singleton.microphone.slab_buffer) {
		LOG_ERR("Failed to allocate microphone slab buffer");
		k_mutex_unlock(&mic_lock);
		return NULL;
	}

	/* Initialize memory slab */
	int ret = k_mem_slab_init(&mic_singleton.microphone.slab,
				  mic_singleton.microphone.slab_buffer,
				  mic_singleton.microphone.block_size,
				  mic_singleton.microphone.block_count);
	if (ret != 0) {
		LOG_ERR("Failed to initialize memory slab: %d", ret);
		halo_free(mic_singleton.microphone.slab_buffer);
		mic_singleton.microphone.slab_buffer = NULL;
		k_mutex_unlock(&mic_lock);
		return NULL;
	}

	/* Configure DMIC */
	mic_singleton.microphone.config.channel.req_num_streams = 1;
	mic_singleton.microphone.config.channel.req_num_chan = channels;
	mic_singleton.microphone.config.channel.req_chan_map_lo = (channels == 2) ? (BIT(2) | BIT(3)) : BIT(2);
	mic_singleton.microphone.config.channel.req_chan_map_hi = 0;

	struct pcm_stream_cfg stream_cfg = {
		.pcm_width = bit_depth,
		.pcm_rate = sample_rate,
		.mem_slab = &mic_singleton.microphone.slab,
		.block_size = mic_singleton.microphone.block_size,
	};
	mic_singleton.microphone.config.streams = &stream_cfg;

	ret = dmic_configure(mic_singleton.microphone.dmic, &mic_singleton.microphone.config);
	if (ret != 0) {
		LOG_ERR("Failed to configure DMIC: %d", ret);
		halo_free(mic_singleton.microphone.slab_buffer);
		mic_singleton.microphone.slab_buffer = NULL;
		k_mutex_unlock(&mic_lock);
		return NULL;
	}

	/* Set gain */
	int saved_gain = gain; /* use provided gain as default */
	if (halo_settings_get("audio/gain", &saved_gain, sizeof(saved_gain)) != 0) {
		/* Use provided gain if not saved */
		saved_gain = gain;
	}
	if (saved_gain < -10) {
		saved_gain = -10;
	} else if (saved_gain > 10) {
		saved_gain = 10;
	}

	dmic_set_gain(mic_singleton.microphone.dmic, saved_gain);
	
	/* Store current gain in structure */
	mic_singleton.microphone.gain = saved_gain;

	mic_singleton.microphone.is_started = false;
	mic_singleton.is_initialized = true;
	mic_singleton.owner = owner;

	LOG_DBG("Microphone acquired by %s", audio_owner_name(owner));

	k_mutex_unlock(&mic_lock);
	return &mic_singleton.microphone;
}

int audio_microphone_start(audio_microphone_t *mic)
{
	if (!mic) {
		return -EINVAL;
	}

	int ret = dmic_trigger(mic->dmic, DMIC_TRIGGER_START);
	if (ret != 0) {
		LOG_ERR("Failed to start DMIC: %d", ret);
		return ret;
	}

	mic_hw_start_ms = k_uptime_get_32();
	mic_first_dmic_read_logged = false;
	LOG_DBG("Mic DMIC start at uptime %u ms (block interval %d ms)",
		mic_hw_start_ms, MIC_BLOCK_INTERVAL_MS);

	mic->is_started = true;
	return 0;
}

int audio_microphone_read(audio_microphone_t *mic, void **buffer, size_t *len)
{
	if (!mic || !buffer || !len) {
		return -EINVAL;
	}

	if (!mic->is_started) {
		return -ENODEV;
	}

	uint32_t size = 0;
	int ret = dmic_read(mic->dmic, 0, buffer, &size, MIC_BLOCK_INTERVAL_MS * 2);
	if (ret != 0) {
		if (ret != -EAGAIN) {
			LOG_ERR("DMIC read error: %d", ret);
		}
		return ret;
	}

	*len = size;

	if (!mic_first_dmic_read_logged) {
		mic_first_dmic_read_logged = true;
		LOG_DBG("Mic first DMIC block: %u bytes at +%u ms",
			size, k_uptime_get_32() - mic_hw_start_ms);
	}

	return 0;
}

void audio_microphone_free_buffer(audio_microphone_t *mic, void *buffer)
{
	if (mic && buffer) {
		k_mem_slab_free(&mic->slab, buffer);
	}
}

int audio_microphone_set_gain(audio_microphone_t *mic, int gain)
{
	if (!mic) {
		return -EINVAL;
	}

	if (gain < -10 || gain > 10) {
		LOG_ERR("Invalid gain: %d (must be -10 to 10)", gain);
		return -EINVAL;
	}

	int ret = dmic_set_gain(mic->dmic, gain);
	if (ret != 0) {
		LOG_ERR("Failed to set gain: %d", ret);
		return ret;
	}

	/* Store current gain in structure */
	mic->gain = gain;

	/* Save gain setting persistently */
	halo_settings_set("audio/gain", &gain, sizeof(gain));

	/* Gain adjustments are frequent; skip verbose logging */
	return 0;
}

int audio_microphone_get_gain(audio_microphone_t *mic)
{
	if (mic) {
		return mic->gain;
	} else {
		/* If no microphone provided, get from settings */
		int gain = 0; /* default */
		if (halo_settings_get("audio/gain", &gain, sizeof(gain)) != 0) {
			gain = 0;
		}
		return gain;
	}
}

int audio_microphone_stop(audio_microphone_t *mic)
{
	if (!mic) {
		return -EINVAL;
	}

	if (!mic->is_started) {
		return 0; /* Already stopped */
	}

	int ret = dmic_trigger(mic->dmic, DMIC_TRIGGER_STOP);
	if (ret != 0) {
		LOG_ERR("Failed to stop DMIC: %d", ret);
		return ret;
	}

	mic->is_started = false;
	return 0;
}

void audio_microphone_destroy(audio_microphone_t *mic)
{
	if (!mic) {
		return;
	}

	k_mutex_lock(&mic_lock, K_FOREVER);

	/* Only destroy if this is the current singleton instance */
	if (mic == &mic_singleton.microphone && mic_singleton.is_initialized) {
		audio_microphone_stop(mic);
		
		if (mic->slab_buffer) {
			halo_free(mic->slab_buffer);
			mic->slab_buffer = NULL;
		}

		LOG_DBG("Microphone released by %s", audio_owner_name(mic_singleton.owner));

		mic_singleton.is_initialized = false;
		mic_singleton.owner = AUDIO_OWNER_NONE;
	}

	k_mutex_unlock(&mic_lock);
}

bool audio_microphone_check_owner(audio_microphone_t *mic, audio_owner_t expected_owner)
{
	if (!mic) {
		return false;
	}

	k_mutex_lock(&mic_lock, K_FOREVER);
	bool result = (mic_singleton.is_initialized &&
		       mic_singleton.owner == expected_owner);
	k_mutex_unlock(&mic_lock);

	return result;
}

audio_owner_t audio_microphone_get_owner(void)
{
	k_mutex_lock(&mic_lock, K_FOREVER);
	audio_owner_t owner =
		mic_singleton.is_initialized ? mic_singleton.owner : AUDIO_OWNER_NONE;
	k_mutex_unlock(&mic_lock);

	return owner;
}

int audio_microphone_read_owned(audio_microphone_t *mic, audio_owner_t owner, void **buffer,
				size_t *len)
{
	if (!audio_microphone_check_owner(mic, owner)) {
		return -EPERM;
	}

	return audio_microphone_read(mic, buffer, len);
}

void audio_microphone_free_buffer_owned(audio_microphone_t *mic, audio_owner_t owner,
					void *buffer)
{
	if (!mic || !buffer) {
		return;
	}

	k_mutex_lock(&mic_lock, K_FOREVER);

	if (mic_singleton.is_initialized && mic_singleton.owner == owner) {
		k_mem_slab_free(&mic->slab, buffer);
	} else if (mic_singleton.is_initialized && mic_singleton.microphone.slab_buffer) {
		/* Ownership was lost while this block was held. If the block belongs to
		 * the current slab (the new owner reads could have handed it out via the
		 * shared driver queue), return it there; otherwise it came from a slab
		 * whose backing memory was already freed wholesale — drop it. */
		uint8_t *base = mic_singleton.microphone.slab_buffer;
		uint8_t *end = base + mic_singleton.microphone.block_size *
					      mic_singleton.microphone.block_count;
		if ((uint8_t *)buffer >= base && (uint8_t *)buffer < end) {
			k_mem_slab_free(&mic_singleton.microphone.slab, buffer);
		}
	}

	k_mutex_unlock(&mic_lock);
}

/* ============================================================================
 * AAD (Acoustic Activity Detection) Implementation
 * ============================================================================ */

struct audio_aad {
	const struct device *dev;
	int threshold_db;
	int silent_period_ms;
	bool is_started;
};
static audio_aad_callback_t aad_callback = NULL;

static void t5838_aad_handler(const struct device *dev)
{
	ARG_UNUSED(dev);
	if (aad_callback) {
		aad_callback();
	}

	t5838_aad_wake_clear(dev);
}

/**
 * @brief Find the closest AAD threshold to the requested value
 *
 * Searches through available hardware thresholds and returns the closest match.
 *
 * @param requested_db Requested threshold in dB SPL
 * @return Closest matching t5838_aad_a_thr enum value
 */
static enum t5838_aad_a_thr find_closest_aad_threshold(int requested_db)
{
	/* Threshold lookup table: {threshold_db, enum_value} */
	static const struct {
		int db;
		enum t5838_aad_a_thr thr;
	} thresholds[] = {
		{60, T5838_AAD_A_THR_60dB},   {65, T5838_AAD_A_THR_65dB},
		{70, T5838_AAD_A_THR_70dB},   {75, T5838_AAD_A_THR_75dB},
		{80, T5838_AAD_A_THR_80dB},   {85, T5838_AAD_A_THR_85dB},
		{90, T5838_AAD_A_THR_90dB},   {95, T5838_AAD_A_THR_95dB},
		{98, T5838_AAD_A_THR_97_5dB}, /* 97.5dB rounded to 98 */
	};

	/* Default to 90dB if invalid input */
	if (requested_db <= 0 || requested_db > 100) {
		return T5838_AAD_A_THR_90dB;
	}

	/* Find closest threshold */
	int closest_idx = 0;
	int min_diff = abs(requested_db - thresholds[0].db);

	for (size_t i = 1; i < ARRAY_SIZE(thresholds); i++) {
		int diff = abs(requested_db - thresholds[i].db);
		if (diff < min_diff) {
			min_diff = diff;
			closest_idx = i;
		}
	}

	return thresholds[closest_idx].thr;
}

int audio_aad_get_current_settings(int *threshold_db, int *silent_period_ms)
{
	const struct device *aad = DEVICE_DT_GET(DT_NODELABEL(t5838));

	if (!device_is_ready(aad)) {
		LOG_ERR("AAD device not ready");
		return -ENODEV;
	}

	/* Retrieve saved settings */
	if (threshold_db) {
		if (halo_settings_get("audio/aad_threshold", threshold_db, sizeof(*threshold_db)) != 0) {
			*threshold_db = 90; /* default */
		}
	}

	if (silent_period_ms) {
		if (halo_settings_get("audio/aad_silent_period", silent_period_ms,
				      sizeof(*silent_period_ms)) != 0) {
			*silent_period_ms = 1000; /* default */
		}
	}

	return 0;
}

int audio_aad_disable(void)
{
	const struct device *aad = DEVICE_DT_GET(DT_NODELABEL(t5838));

	if (!device_is_ready(aad)) {
		LOG_ERR("AAD device not ready");
		return -ENODEV;
	}
	LOG_WRN("Disabling AAD");

	aad_callback = NULL;
	t5838_aad_wake_handler_set(aad, NULL);
	t5838_aad_mode_disable(aad);

	return 0;
}

int audio_aad_set_callback(audio_aad_callback_t callback, int threshold_db, int silent_period_ms)
{
	const struct device *aad = DEVICE_DT_GET(DT_NODELABEL(t5838));

	if (!device_is_ready(aad)) {
		LOG_ERR("AAD device not ready");
		return -ENODEV;
	}

	aad_callback = callback;

	if (callback) {
		/* Find closest threshold */
		enum t5838_aad_a_thr selected_thr = find_closest_aad_threshold(threshold_db);

		/* Use default silent period if not specified or invalid */
		uint32_t silent_period = (silent_period_ms > 0) ? silent_period_ms : 1000;

		/* Configure AAD */
		struct t5838_aad_a_conf aad_cfg = {
			.aad_a_lpf = T5838_AAD_A_LPF_2_0kHz,
			.aad_a_thr = selected_thr,
			.silent_period = silent_period,
		};
		int ret = t5838_aad_a_mode_set(aad, &aad_cfg);
		if (ret < 0) {
			LOG_ERR("Failed to configure AAD: %d", ret);
			return ret;
		}
		t5838_aad_wake_handler_set(aad, t5838_aad_handler);

		/* Save AAD settings persistently */
		halo_settings_set("audio/aad_threshold", &threshold_db, sizeof(threshold_db));
		halo_settings_set("audio/aad_silent_period", &silent_period_ms,
				  sizeof(silent_period_ms));

	} else {
		t5838_aad_wake_handler_set(aad, NULL);
	}

	return 0;
}

int audio_aad_get_state(bool *cb_installed, int *aad_enabled_mode,
			int *wake_pin_level, bool *int_handled, bool *cb_configured)
{
	const struct device *aad = DEVICE_DT_GET(DT_NODELABEL(t5838));

	if (!device_is_ready(aad)) {
		LOG_ERR("AAD device not ready");
		return -ENODEV;
	}

	const struct t5838_aad_drv_cfg *drv_cfg = aad->config;
	struct t5838_aad_drv_data *drv_data = aad->data;

	if (cb_installed) {
		*cb_installed = (aad_callback != NULL);
	}
	if (aad_enabled_mode) {
		*aad_enabled_mode = (int)drv_data->aad_enabled_mode;
	}
	if (wake_pin_level) {
		*wake_pin_level = gpio_pin_get_dt(&drv_cfg->wake);
	}
	if (int_handled) {
		*int_handled = drv_data->int_handled;
	}
	if (cb_configured) {
		*cb_configured = drv_data->cb_configured;
	}

	return 0;
}

int audio_aad_rearm(void)
{
	const struct device *aad = DEVICE_DT_GET(DT_NODELABEL(t5838));

	if (!device_is_ready(aad)) {
		LOG_ERR("AAD device not ready");
		return -ENODEV;
	}

	if (!aad_callback) {
		return 0;
	}

	int threshold_db = 90;
	int silent_period_ms = 1000;
	audio_aad_get_current_settings(&threshold_db, &silent_period_ms);

	enum t5838_aad_a_thr selected_thr = find_closest_aad_threshold(threshold_db);
	uint32_t silent_period = (silent_period_ms > 0) ? silent_period_ms : 1000;

	struct t5838_aad_a_conf aad_cfg = {
		.aad_a_lpf = T5838_AAD_A_LPF_2_0kHz,
		.aad_a_thr = selected_thr,
		.silent_period = silent_period,
	};

	int ret = 0;
	for (int attempt = 0; attempt < 3; attempt++) {
		ret = t5838_aad_a_mode_set(aad, &aad_cfg);
		if (ret == 0) {
			break;
		}

		if (attempt < 2) {
			k_sleep(K_MSEC(20));
		}
	}

	if (ret < 0) {
		LOG_ERR("Failed to re-arm AAD: %d", ret);
		return ret;
	}

	t5838_aad_wake_handler_set(aad, t5838_aad_handler);

	return 0;
}

/* ============================================================================
 * Audio Transport Callback Management
 * ============================================================================ */

#define MAX_AUDIO_SOURCES 4
#define MAX_AUDIO_SINKS   4

struct audio_source_handle {
	audio_source_callback_t callback;
	void *user_data;
	int priority;
	const char *name;
	bool active;
};

struct audio_sink_handle {
	audio_sink_callback_t callback;
	void *user_data;
	int priority;
	const char *name;
	bool active;
};

static struct {
	struct audio_source_handle sources[MAX_AUDIO_SOURCES];
	struct audio_sink_handle sinks[MAX_AUDIO_SINKS];
	struct k_mutex source_lock;
	struct k_mutex sink_lock;
	int next_source_idx; /* Round-robin index for sources with same priority */
} audio_transport = {
	.next_source_idx = 0,
};

static bool transport_initialized = false;

static void audio_transport_init(void)
{
	if (transport_initialized) {
		return;
	}

	k_mutex_init(&audio_transport.source_lock);
	k_mutex_init(&audio_transport.sink_lock);

	for (int i = 0; i < MAX_AUDIO_SOURCES; i++) {
		audio_transport.sources[i].active = false;
	}

	for (int i = 0; i < MAX_AUDIO_SINKS; i++) {
		audio_transport.sinks[i].active = false;
	}

	transport_initialized = true;
}

audio_source_handle_t audio_stream_register_source(audio_source_callback_t callback,
						   void *user_data, int priority, const char *name)
{
	if (!callback) {
		return NULL;
	}

	audio_transport_init();

	k_mutex_lock(&audio_transport.source_lock, K_FOREVER);

	/* Find free slot */
	struct audio_source_handle *handle = NULL;
	for (int i = 0; i < MAX_AUDIO_SOURCES; i++) {
		if (!audio_transport.sources[i].active) {
			handle = &audio_transport.sources[i];
			break;
		}
	}

	if (!handle) {
		LOG_ERR("No free audio source slots");
		k_mutex_unlock(&audio_transport.source_lock);
		return NULL;
	}

	/* Register source */
	handle->callback = callback;
	handle->user_data = user_data;
	handle->priority = priority;
	handle->name = name ? name : "unnamed";
	handle->active = true;

	k_mutex_unlock(&audio_transport.source_lock);

	return handle;
}

int audio_stream_unregister_source(audio_source_handle_t handle)
{
	if (!handle) {
		return -EINVAL;
	}

	audio_transport_init();

	k_mutex_lock(&audio_transport.source_lock, K_FOREVER);

	if (handle->active) {
		handle->active = false;
	}

	k_mutex_unlock(&audio_transport.source_lock);

	return 0;
}

size_t audio_stream_source_read(uint8_t *data, size_t len, k_timeout_t timeout)
{
	if (!data || len == 0) {
		return 0;
	}

	audio_transport_init();

	k_mutex_lock(&audio_transport.source_lock, K_FOREVER);

	/* Build sorted list of active sources by priority */
	struct audio_source_handle *sorted[MAX_AUDIO_SOURCES];
	int count = 0;

	for (int i = 0; i < MAX_AUDIO_SOURCES; i++) {
		if (audio_transport.sources[i].active) {
			sorted[count++] = &audio_transport.sources[i];
		}
	}

	if (count == 0) {
		k_mutex_unlock(&audio_transport.source_lock);
		k_sleep(timeout);
		return 0; /* No sources available */
	}

	/* Sort by priority (lower number = higher priority) */
	for (int i = 0; i < count - 1; i++) {
		for (int j = i + 1; j < count; j++) {
			if (sorted[j]->priority < sorted[i]->priority) {
				struct audio_source_handle *temp = sorted[i];
				sorted[i] = sorted[j];
				sorted[j] = temp;
			}
		}
	}

	/* Try each source in priority order */
	size_t bytes_read = 0;
	for (int i = 0; i < count; i++) {
		struct audio_source_handle *src = sorted[i];

		/* Release lock during callback to avoid deadlock */
		k_mutex_unlock(&audio_transport.source_lock);

		bytes_read = src->callback(data, len, timeout, src->user_data);

		k_mutex_lock(&audio_transport.source_lock, K_FOREVER);

		if (bytes_read > 0) {
			break; /* Got data, stop trying */
		}
	}

	k_mutex_unlock(&audio_transport.source_lock);

	return bytes_read;
}

audio_sink_handle_t audio_stream_register_sink(audio_sink_callback_t callback, void *user_data,
					       int priority, const char *name)
{
	if (!callback) {
		return NULL;
	}

	audio_transport_init();

	k_mutex_lock(&audio_transport.sink_lock, K_FOREVER);

	/* Find free slot */
	struct audio_sink_handle *handle = NULL;
	for (int i = 0; i < MAX_AUDIO_SINKS; i++) {
		if (!audio_transport.sinks[i].active) {
			handle = &audio_transport.sinks[i];
			break;
		}
	}

	if (!handle) {
		LOG_ERR("No free audio sink slots");
		k_mutex_unlock(&audio_transport.sink_lock);
		return NULL;
	}

	/* Register sink */
	handle->callback = callback;
	handle->user_data = user_data;
	handle->priority = priority;
	handle->name = name ? name : "unnamed";
	handle->active = true;

	k_mutex_unlock(&audio_transport.sink_lock);

	return handle;
}

int audio_stream_unregister_sink(audio_sink_handle_t handle)
{
	if (!handle) {
		return -EINVAL;
	}

	audio_transport_init();

	k_mutex_lock(&audio_transport.sink_lock, K_FOREVER);

	if (handle->active) {
		handle->active = false;
	}

	k_mutex_unlock(&audio_transport.sink_lock);

	return 0;
}

size_t audio_stream_sink_write(const uint8_t *data, size_t len)
{
	if (!data || len == 0) {
		return 0;
	}

	audio_transport_init();

	k_mutex_lock(&audio_transport.sink_lock, K_FOREVER);

	/* Build sorted list of active sinks by priority */
	struct audio_sink_handle *sorted[MAX_AUDIO_SINKS];
	int count = 0;

	for (int i = 0; i < MAX_AUDIO_SINKS; i++) {
		if (audio_transport.sinks[i].active) {
			sorted[count++] = &audio_transport.sinks[i];
		}
	}

	if (count == 0) {
		k_mutex_unlock(&audio_transport.sink_lock);
		return 0; /* No sinks available */
	}

	/* Sort by priority (lower number = higher priority) */
	for (int i = 0; i < count - 1; i++) {
		for (int j = i + 1; j < count; j++) {
			if (sorted[j]->priority < sorted[i]->priority) {
				struct audio_sink_handle *temp = sorted[i];
				sorted[i] = sorted[j];
				sorted[j] = temp;
			}
		}
	}

	/* Broadcast to all sinks */
	size_t total_written = 0;
	for (int i = 0; i < count; i++) {
		struct audio_sink_handle *sink = sorted[i];

		/* Release lock during callback to avoid deadlock */
		k_mutex_unlock(&audio_transport.sink_lock);

		size_t written = sink->callback(data, len, sink->user_data);

		k_mutex_lock(&audio_transport.sink_lock, K_FOREVER);

		if (written > 0) {
			total_written += written;
		}
	}

	k_mutex_unlock(&audio_transport.sink_lock);

	return total_written;
}
