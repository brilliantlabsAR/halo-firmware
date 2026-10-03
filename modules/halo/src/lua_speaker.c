/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "lua.h"
#include "lauxlib.h"

#include <halo/audio_stream.h>
#include <halo/mem_manager.h>
#include <halo/lua_microphone.h>
#include <halo/lua_service.h>
#include <halo/lua_runtime.h>
#include <halo/pm_manager.h>
#include <halo/file_manager.h>
#include <halo/ble_lua.h>

#if defined(CONFIG_MAX98357A_AUDIO)
#include <max98357a_audio.h>
#endif

LOG_MODULE_REGISTER(lua_speaker, CONFIG_HALO_LOG_LEVEL);

#define LUA_SPEAKER_AUDIO_BUFFER_SIZE 480 // 20 30 40 60

/* ============================================================================
 * Speaker State Management
 * ============================================================================ */

/**
 * @brief Speaker state with integrated thread and resource management
 *
 * Centralizes all speaker-related state for lifecycle management
 */
static struct {
	/* Audio hardware */
	audio_speaker_t *speaker;

/* LC3 decoders - one per channel for stereo support */
#define MAX_SPEAKER_CHANNELS 2
	audio_codec_ctx_t *lc3_decoder[MAX_SPEAKER_CHANNELS];

	/* Thread management */
	k_tid_t thread_tid;
	struct k_thread thread;
	K_THREAD_STACK_MEMBER(stack, CONFIG_HALO_LUA_SPEAKER_TASK_STACK_SIZE);
	struct k_sem sem;
	struct k_sem stream_exit_sem; /* Signals when thread exits streaming loop */
	bool thread_should_exit;

	/* Configuration */
	bool use_lc3;
	bool is_streaming;
	int sample_rate;
	int channel_count;
	int lc3_duration;
	int lc3_bitrate;

	/* Buffers */
	uint8_t *ble_buffer;
	int16_t *pcm_buffer;     /* Interleaved output buffer for all channels */
	int16_t *channel_buffer; /* Single channel buffer for decoding */
	size_t pcm_frame_size;   /* Per-channel frame size */

	/* Synchronization */
	struct k_mutex mutex;
} speaker_state = {
	.speaker = NULL,
	.lc3_decoder = {NULL, NULL},
	.thread_tid = NULL,
	.thread_should_exit = false,
	.use_lc3 = false,
	.is_streaming = false,
	.channel_count = 1,
	.ble_buffer = NULL,
	.pcm_buffer = NULL,
	.channel_buffer = NULL,
};

/* ============================================================================
 * Playback Accounting (frame.speaker.stats)
 * ============================================================================ */

/* Monotonic from boot; stats(true) moves the baselines instead of zeroing,
 * so nothing here is written by more than one context at a time: the
 * decode/write counters change under speaker_state.mutex (pump thread or
 * play()), starts/restarts on the Lua thread.
 */
struct speaker_counters {
	uint32_t starts;
	uint32_t restarts;
	uint32_t frames_decoded;
	uint32_t frames_plc;
	uint32_t frames_muted;
	uint32_t mute_events;
	uint32_t decode_errors;
	uint32_t bytes_misaligned;
	uint32_t frames_dropped_stop;
	uint32_t frames_write_failed;
	uint32_t pcm_bytes;
	uint32_t pcm_bytes_failed;
};

static struct speaker_counters spk_cnt;
static struct speaker_counters spk_base;
static struct halo_ble_lua_audio_rx_stats rx_base;
#if defined(CONFIG_MAX98357A_AUDIO)
static struct max98357a_audio_tx_diag amp_base;
#endif

/* ============================================================================
 * Speaker Background Thread
 * ============================================================================ */

/**
 * @brief Speaker playback thread
 *
 * Continuously reads audio data from BLE and writes to speaker hardware.
 * Handles LC3 decoding if enabled.
 */
/* Decode one channel's LC3 frame into channel_buffer, counting the result.
 * Caller holds speaker_state.mutex.
 */
static int speaker_decode(int ch, const uint8_t *frame, size_t frame_size)
{
	audio_codec_ctx_t *dec = speaker_state.lc3_decoder[ch];
	int ret = audio_lc3_decode_frame(dec, frame, frame_size,
					 speaker_state.channel_buffer,
					 speaker_state.pcm_frame_size);

	if (ret != 0) {
		spk_cnt.decode_errors++;
		return ret;
	}

	uint8_t flags = audio_lc3_decoder_frame_flags(dec);

	spk_cnt.frames_decoded++;
	if (flags & AUDIO_LC3_FRAME_MUTED) {
		spk_cnt.frames_muted++;
	} else if (flags & AUDIO_LC3_FRAME_BAD) {
		spk_cnt.frames_plc++;
	}
	if (flags & AUDIO_LC3_FRAME_MUTE_ENGAGED) {
		spk_cnt.mute_events++;
	}
	return 0;
}

/* Hand PCM to the speaker, counting anything it refuses (stopped,
 * suspended, or preempted by LE Audio). Caller holds speaker_state.mutex.
 */
static void speaker_write(uint8_t *pcm, size_t len, bool lc3)
{
	int ret = audio_speaker_write(speaker_state.speaker, pcm, len);
	size_t written = ret > 0 ? (size_t)ret : 0;

	if (lc3) {
		if (written < len) {
			spk_cnt.frames_write_failed++;
		}
	} else {
		spk_cnt.pcm_bytes += written;
		spk_cnt.pcm_bytes_failed += len - MIN(written, len);
	}
}

static void speaker_thread_fn(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (!speaker_state.thread_should_exit) {
		/* Wait for start signal */
		k_sem_take(&speaker_state.sem, K_FOREVER);

		if (speaker_state.thread_should_exit) {
			break;
		}

		while (speaker_state.is_streaming && !speaker_state.thread_should_exit) {
			/* Read audio data from BLE */
			int32_t len =
				halo_ble_lua_audio_read(speaker_state.ble_buffer,
							LUA_SPEAKER_AUDIO_BUFFER_SIZE, K_MSEC(500));

			if (len <= 0) {
				k_sleep(K_MSEC(10));
				continue;
			}

			k_mutex_lock(&speaker_state.mutex, K_FOREVER);

			/* Check state again after acquiring mutex */
			if (!speaker_state.is_streaming || speaker_state.thread_should_exit) {
				if (speaker_state.use_lc3) {
					size_t fs = audio_lc3_get_frame_size(
						speaker_state.sample_rate,
						speaker_state.lc3_duration,
						speaker_state.lc3_bitrate) *
						speaker_state.channel_count;

					if (fs > 0) {
						spk_cnt.frames_dropped_stop += len / fs;
					}
				}
				k_mutex_unlock(&speaker_state.mutex);
				break;
			}

			if (speaker_state.use_lc3) {
				/* LC3 decoding path - handle multiple channels */
				size_t frame_size = audio_lc3_get_frame_size(
					speaker_state.sample_rate, speaker_state.lc3_duration,
					speaker_state.lc3_bitrate);

				if (frame_size > 0) {
					/* Total frames = data / (frame_size * channels) */
					size_t frame_size_total =
						frame_size * speaker_state.channel_count;
					size_t num_frames = len / frame_size_total;
					size_t samples_per_frame =
						speaker_state.pcm_frame_size / sizeof(int16_t);

					/* A read that is not whole frames leaves a
					 * tail that is discarded here (and misaligns
					 * the frames after it).
					 */
					spk_cnt.bytes_misaligned += len % frame_size_total;

					for (size_t i = 0; i < num_frames; i++) {
						/* Decode each channel separately */
						for (int ch = 0; ch < speaker_state.channel_count;
						     ch++) {
							size_t offset = i * frame_size_total +
									ch * frame_size;

							/* Decode to single-channel buffer */
							int ret = speaker_decode(
								ch,
								speaker_state.ble_buffer + offset,
								frame_size);

							if (ret != 0) {
								LOG_ERR("LC3 decode failed for "
									"ch%d: %d",
									ch, ret);
								continue;
							}

							/* Interleave into output buffer: LRLRLR...
							 */
							for (size_t j = 0; j < samples_per_frame;
							     j++) {
								speaker_state.pcm_buffer
									[j * speaker_state
											 .channel_count +
									 ch] =
									speaker_state
										.channel_buffer[j];
							}
						}

						/* Write interleaved PCM to speaker */
						size_t total_pcm_size =
							speaker_state.pcm_frame_size *
							speaker_state.channel_count;
						speaker_write(
							(uint8_t *)speaker_state.pcm_buffer,
							total_pcm_size, true);

						if (speaker_state.thread_should_exit ||
						    !speaker_state.is_streaming) {
							/* rest of this read is lost */
							spk_cnt.frames_dropped_stop +=
								num_frames - i - 1;
							break;
						}
					}
				}
			} else {
				/* PCM direct playback */
				speaker_write(speaker_state.ble_buffer, len, false);
			}

			k_mutex_unlock(&speaker_state.mutex);
		}

		/* Signal that we've exited the streaming loop */
		k_sem_give(&speaker_state.stream_exit_sem);
	}
}

/* ============================================================================
 * Resource Cleanup
 * ============================================================================ */

/**
 * @brief Stop hardware and clean up audio resources (not thread)
 */
static void speaker_cleanup_audio_resources(void)
{
	/* Stop and destroy audio hardware only if still owned by Lua */
	if (speaker_state.speaker) {
		/* Check if still owned by Lua before destroying */
		if (audio_speaker_check_owner(speaker_state.speaker, AUDIO_OWNER_LUA)) {
			audio_speaker_stop(speaker_state.speaker);
			audio_speaker_destroy(speaker_state.speaker);
		}
		/* Clear pointer regardless (it may have been preempted) */
		speaker_state.speaker = NULL;
	}

	/* Destroy all LC3 decoders (Lua-owned resources) */
	for (int ch = 0; ch < MAX_SPEAKER_CHANNELS; ch++) {
		if (speaker_state.lc3_decoder[ch]) {
			audio_lc3_decoder_destroy(speaker_state.lc3_decoder[ch]);
			speaker_state.lc3_decoder[ch] = NULL;
		}
	}

	/* Free buffers (Lua-owned resources) */
	if (speaker_state.ble_buffer) {
		halo_free(speaker_state.ble_buffer);
		speaker_state.ble_buffer = NULL;
	}

	if (speaker_state.pcm_buffer) {
		halo_free(speaker_state.pcm_buffer);
		speaker_state.pcm_buffer = NULL;
	}

	if (speaker_state.channel_buffer) {
		halo_free(speaker_state.channel_buffer);
		speaker_state.channel_buffer = NULL;
	}

	/* Reset state flags */
	speaker_state.use_lc3 = false;
}

/**
 * @brief Stop playback thread safely
 */
static void speaker_stop_thread(void)
{
	if (!speaker_state.thread_tid) {
		return;
	}

	/* First exit streaming loop if active */
	if (speaker_state.is_streaming) {
		speaker_state.is_streaming = false;
		k_sem_take(&speaker_state.stream_exit_sem, K_FOREVER);
	}

	/* Then stop the thread completely */
	speaker_state.thread_should_exit = true;

	/* Wake up thread if waiting on semaphore */
	k_sem_give(&speaker_state.sem);

	/* Wait for thread to exit */
	int ret = k_thread_join(speaker_state.thread_tid, K_MSEC(1000));
	if (ret != 0) {
		LOG_WRN("Thread join timeout, aborting");
		k_thread_abort(speaker_state.thread_tid);
		/* Reap aborted thread before reusing thread state. */
		k_thread_join(speaker_state.thread_tid, K_FOREVER);
	}

	speaker_state.thread_tid = NULL;
	speaker_state.thread_should_exit = false;
	k_sem_reset(&speaker_state.sem);
}


/* ============================================================================
 * Lua API Implementation
 * ============================================================================ */

/**
 * @brief frame.speaker.start(config)
 *
 * Start speaker playback with configuration.
 * Can be called multiple times to reconfigure (will stop and restart).
 *
 * @param config Table with fields:
 *   - encoder: "pcm" or "lc3" (default: "pcm")
 *   - sample_rate: 8000 or 16000 (default: 8000)
 *   - channels: 1 (mono) or 2 (stereo) (default: 1)
 *   - bit_depth: 16 only (default: 16, PCM only; 8-bit input is not
 *     supported — the playback path is int16 throughout)
 *   - duration: 750 or 1000 (default: 1000, LC3 only)
 *   - bitrate: 8000-96000 (default: 16000, LC3 only)
 *   - volume: 0-100 (default: 50)
 *   - gain: 0-12 dB digital pre-gain into the protection limiter
 *     (default: 0). Lifts quiet sources (e.g. un-normalized TTS) toward
 *     the loudness ceiling; hot sources are compressed instead of
 *     getting louder. Transient: applies to this stream only.
 *   - budget: 10-100, per-stream energy-budget override (default:
 *     the configured budget). Higher = louder ceiling, more battery
 *     current; any override engages the fast limiter attack that
 *     makes raised budgets safe. Clamped to the Kconfig maximum. All
 *     protection stays active. budget=100 with gain=12 is the loudest
 *     supported voice. Transient per stream.
 *
 * For LC3 stereo: Input format is [Left LC3][Right LC3]...
 * Output PCM will be interleaved LRLRLR...
 */
static int lua_speaker_start(lua_State *L)
{
	if (!lua_istable(L, 1)) {
		return luaL_error(L, "Expected a configuration table");
	}

	LOG_DBG("Speaker start called");

	/* If already running, stop first to reconfigure */
	if (speaker_state.is_streaming) {
		spk_cnt.restarts++;

		/* Exit streaming loop first */
		speaker_state.is_streaming = false;
		k_sem_take(&speaker_state.stream_exit_sem, K_FOREVER);
		
		/* Then cleanup resources */
		speaker_cleanup_audio_resources();
	}

	/* Parse configuration with defaults */
	const char *encoder = "pcm";
	int sample_rate = 8000;
	int bit_depth = 16;
	int channels = 1;
	int volume = 50;
	int lc3_duration = 1000;
	int lc3_bitrate = 16000;

	/* encoder */
	lua_getfield(L, 1, "encoder");
	if (lua_isstring(L, -1)) {
		encoder = lua_tostring(L, -1);
	}
	lua_pop(L, 1);

	speaker_state.use_lc3 = (strcmp(encoder, "lc3") == 0);

	/* sample_rate */
	lua_getfield(L, 1, "sample_rate");
	if (lua_isnumber(L, -1)) {
		sample_rate = lua_tointeger(L, -1);
	}
	lua_pop(L, 1);

	if (sample_rate != 8000 && sample_rate != 16000) {
		return luaL_error(L, "Sample rate must be 8000 or 16000");
	}

	/* channels */
	lua_getfield(L, 1, "channels");
	if (lua_isnumber(L, -1)) {
		channels = lua_tointeger(L, -1);
	}
	lua_pop(L, 1);

	if (channels < 1 || channels > MAX_SPEAKER_CHANNELS) {
		return luaL_error(L, "Channels must be 1 or 2");
	}

	/* bit_depth */
	lua_getfield(L, 1, "bit_depth");
	if (lua_isnumber(L, -1)) {
		bit_depth = lua_tointeger(L, -1);
	}
	lua_pop(L, 1);
	if (bit_depth != 16) {
		return luaL_error(L, "Bit depth must be 16");
	}

	/* LC3 parameters */
	if (speaker_state.use_lc3) {
		lua_getfield(L, 1, "duration");
		if (lua_isnumber(L, -1)) {
			lc3_duration = lua_tointeger(L, -1);
		}
		lua_pop(L, 1);

		lua_getfield(L, 1, "bitrate");
		if (lua_isnumber(L, -1)) {
			lc3_bitrate = lua_tointeger(L, -1);
		}
		lua_pop(L, 1);

		if (lc3_duration != 750 && lc3_duration != 1000) {
			return luaL_error(L, "LC3 duration must be 750 or 1000");
		}

		if (lc3_bitrate % 8000 != 0 || lc3_bitrate > 96000) {
			return luaL_error(L, "Bitrate must be multiple of 8000 and <= 96000");
		}
	}

	/* volume */
	lua_getfield(L, 1, "volume");
	if (lua_isnumber(L, -1)) {
		volume = lua_tointeger(L, -1);
	}
	lua_pop(L, 1);

	if (volume < 0 || volume > 100) {
		return luaL_error(L, "Volume must be 0-100");
	}

	/* gain (per-stream pre-gain into the protection limiter, dB) */
	int gain_db = 0;

	lua_getfield(L, 1, "gain");
	if (lua_isnumber(L, -1)) {
		gain_db = lua_tointeger(L, -1);
	}
	lua_pop(L, 1);

	if (gain_db < 0 || gain_db > 12) {
		return luaL_error(L, "Gain must be 0-12 dB");
	}

	/* budget (per-stream energy-budget override) */
	int budget = 0;

	lua_getfield(L, 1, "budget");
	if (lua_isnumber(L, -1)) {
		budget = lua_tointeger(L, -1);
	}
	lua_pop(L, 1);

	if (budget != 0 && (budget < 10 || budget > 100)) {
		return luaL_error(L, "Budget must be 10-100");
	}

	/* Save configuration */
	speaker_state.sample_rate = sample_rate;
	speaker_state.channel_count = channels;
	speaker_state.lc3_duration = lc3_duration;																											
	speaker_state.lc3_bitrate = lc3_bitrate;

	/* Initialize speaker hardware with channel configuration */
	speaker_state.speaker = audio_speaker_init(sample_rate, bit_depth, channels,
						   AUDIO_OWNER_LUA);
	if (!speaker_state.speaker) {
		return luaL_error(L, "Failed to initialize speaker (may be occupied by LE Audio)");
	}

	/* The budget override must land before the stream starts (the
	 * protection chain rebuilds while the stream is stopped).
	 */
	if (budget > 0) {
		audio_speaker_set_stream_budget(speaker_state.speaker, budget);
	}

	/* Initialize LC3 decoder if needed */
	if (speaker_state.use_lc3) {
		/* Create one decoder per channel (LC3 is per-channel) */
		for (int ch = 0; ch < channels; ch++) {
			speaker_state.lc3_decoder[ch] =
				audio_lc3_decoder_create(sample_rate, lc3_duration, lc3_bitrate);

			if (!speaker_state.lc3_decoder[ch]) {
				speaker_cleanup_audio_resources();
				return luaL_error(L, "Failed to create LC3 decoder for channel %d",
						  ch);
			}
		}

		/* PCM frame size is per channel */
		speaker_state.pcm_frame_size =
			audio_lc3_get_pcm_frame_size(sample_rate, lc3_duration);

		/* Allocate PCM buffer for all channels (interleaved output) */
		speaker_state.pcm_buffer =
			halo_malloc(speaker_state.pcm_frame_size * channels, HALO_MEM_REGION_AUTO);

		if (!speaker_state.pcm_buffer) {
			speaker_cleanup_audio_resources();
			return luaL_error(L, "Failed to allocate PCM buffer");
		}

		/* Allocate single-channel buffer for decoding */
		speaker_state.channel_buffer =
			halo_malloc(speaker_state.pcm_frame_size, HALO_MEM_REGION_AUTO);

		if (!speaker_state.channel_buffer) {
			speaker_cleanup_audio_resources();
			return luaL_error(L, "Failed to allocate channel buffer");
		}
	}

	/* Allocate BLE receive buffer */
	speaker_state.ble_buffer = halo_malloc(LUA_SPEAKER_AUDIO_BUFFER_SIZE, HALO_MEM_REGION_AUTO);
	if (!speaker_state.ble_buffer) {
		speaker_cleanup_audio_resources();
		return luaL_error(L, "Failed to allocate BLE buffer");
	}

	/* Start speaker */
	int ret = audio_speaker_start(speaker_state.speaker);
	if (ret != 0) {
		speaker_cleanup_audio_resources();
		return luaL_error(L, "Failed to start speaker: %d", ret);
	}

	/* Set volume */
	audio_speaker_set_volume(speaker_state.speaker, volume);

	/* Per-stream pre-gain (audio_speaker_init reset it to unity) */
	if (gain_db > 0) {
		audio_speaker_set_stream_gain(speaker_state.speaker, gain_db * 10);
	}

	/* Start background thread if not already running */
	if (!speaker_state.thread_tid) {
		speaker_state.thread_should_exit = false;

		speaker_state.thread_tid = k_thread_create(
			&speaker_state.thread, speaker_state.stack,
			K_THREAD_STACK_SIZEOF(speaker_state.stack), speaker_thread_fn, NULL, NULL,
			NULL, CONFIG_HALO_LUA_SPEAKER_TASK_PRIORITY, 0, K_NO_WAIT);

		k_thread_name_set(&speaker_state.thread, "lua_speaker");
	}

	speaker_state.is_streaming = true;
	spk_cnt.starts++;
	k_sem_give(&speaker_state.sem);

	LOG_DBG("Speaker: %s %dHz %dbit vol=%d bitrate:%d duration:%d",
		speaker_state.use_lc3 ? "LC3" : "PCM", sample_rate, bit_depth, volume, lc3_bitrate,
		lc3_duration);

	return 0;
}

/**
 * @brief frame.speaker.stop()
 *
 * Stop speaker playback and clean up resources.
 * Thread is kept alive for future start() calls.
 */
static int lua_speaker_stop(lua_State *L)
{
	ARG_UNUSED(L);

	if (!speaker_state.is_streaming) {
		return 0; /* Already stopped */
	}

	/* Exit streaming loop first */
	speaker_state.is_streaming = false;
	k_sem_take(&speaker_state.stream_exit_sem, K_FOREVER);

	/* Then cleanup audio resources */
	speaker_cleanup_audio_resources();

	LOG_DBG("Speaker stopped");

	return 0;
}

/**
 * @brief frame.speaker.play(data)
 *
 * Directly play audio data (PCM or LC3 encoded).
 * This function bypasses the BLE streaming thread.
 *
 * @param data String containing audio data
 */
static int lua_speaker_play(lua_State *L)
{
	size_t len = 0;
	const uint8_t *data = (const uint8_t *)luaL_checklstring(L, 1, &len);

	if (!speaker_state.is_streaming) {
		return luaL_error(L, "Speaker not started");
	}

	if (len == 0) {
		return 0;
	}

	k_mutex_lock(&speaker_state.mutex, K_FOREVER);

	if (speaker_state.use_lc3) {
		/* LC3 decoding path - handle multiple channels */
		size_t frame_size = audio_lc3_get_frame_size(speaker_state.sample_rate,
							     speaker_state.lc3_duration,
							     speaker_state.lc3_bitrate);

		size_t frame_size_total = frame_size * speaker_state.channel_count;

		if (frame_size == 0 || len % frame_size_total != 0) {
			k_mutex_unlock(&speaker_state.mutex);
			return luaL_error(L,
					  "Invalid LC3 data size: %d (expected multiple of %d)",
					  (int)len, (int)frame_size_total);
		}

		size_t num_frames = len / frame_size_total;
		size_t samples_per_frame = speaker_state.pcm_frame_size / sizeof(int16_t);

		for (size_t i = 0; i < num_frames; i++) {
			/* Decode each channel separately */
			for (int ch = 0; ch < speaker_state.channel_count; ch++) {
				size_t offset = i * frame_size_total + ch * frame_size;

				/* Decode to single-channel buffer */
				int ret = speaker_decode(ch, data + offset, frame_size);

				if (ret != 0) {
					k_mutex_unlock(&speaker_state.mutex);
					return luaL_error(L, "LC3 decode error ch%d: %d", ch, ret);
				}

				/* Interleave into output buffer: LRLRLR... */
				for (size_t j = 0; j < samples_per_frame; j++) {
					speaker_state
						.pcm_buffer[j * speaker_state.channel_count + ch] =
						speaker_state.channel_buffer[j];
				}
			}

			/* Write interleaved PCM to speaker */
			size_t total_pcm_size =
				speaker_state.pcm_frame_size * speaker_state.channel_count;
			speaker_write((uint8_t *)speaker_state.pcm_buffer,
				      total_pcm_size, true);
		}
	} else {
		/* PCM direct playback - cast needed as HPF may modify buffer in-place.
		 * The Lua string data is heap-allocated and consumed immediately. */
		speaker_write((uint8_t *)data, len, false);
	}

	k_mutex_unlock(&speaker_state.mutex);

	return 0;
}

/**
 * @brief frame.speaker.volume([level])
 *
 * Get or set speaker volume.
 *
 * @param level Optional volume level 0-100. If not provided, returns current volume.
 * @return If getting: current volume level. If setting: nothing.
 */
static int lua_speaker_volume(lua_State *L)
{
	/* Check if argument provided (setter) or not (getter) */
	if (lua_gettop(L) == 0) {
		/* Getter: return current volume */
		int volume = audio_speaker_get_volume(NULL);
		LOG_DBG("Speaker volume get: %d", volume);
		lua_pushinteger(L, volume);
		return 1;
	} else {
		/* Setter: set volume */
		if (!speaker_state.speaker) {
			return luaL_error(L, "Speaker not initialized");
		}

		int volume = luaL_checkinteger(L, 1);

		if (volume < 0 || volume > 100) {
			return luaL_error(L, "Volume must be 0-100");
		}

		k_mutex_lock(&speaker_state.mutex, K_FOREVER);
		int ret = audio_speaker_set_volume(speaker_state.speaker, volume);
		k_mutex_unlock(&speaker_state.mutex);

		if (ret != 0) {
			return luaL_error(L, "Failed to set volume: %d", ret);
		}

		LOG_DBG("Speaker volume set: %d", volume);
		return 0;
	}
}

#define STAT(name, val)                                                        \
	do {                                                                   \
		lua_pushinteger(L, (lua_Integer)(val));                        \
		lua_setfield(L, -2, name);                                     \
	} while (0)

/**
 * @brief frame.speaker.stats([reset])
 *
 * Playback accounting along the whole speaker path, so a lost stretch of
 * audio can be placed: never arrived (ble_rejected*), arrived but not
 * decoded (bytes_misaligned, decode_errors, frames_dropped_stop), decoded
 * but silenced or refused (frames_muted, frames_write_failed), or queued
 * but thrown away at a stop (blocks_discarded). Cheap counters only - no
 * logging. Counts cover the window since boot or the last stats(true).
 *
 * @param reset true: return this window's counts and start a new window
 * @return table of integer counters plus `streaming` (boolean)
 */
static int lua_speaker_stats(lua_State *L)
{
	bool reset = lua_toboolean(L, 1);
	struct speaker_counters c = spk_cnt;
	struct halo_ble_lua_audio_rx_stats rx;

	halo_ble_lua_audio_rx_stats_get(&rx);

	lua_createtable(L, 0, 26);

	lua_pushboolean(L, speaker_state.is_streaming);
	lua_setfield(L, -2, "streaming");
	STAT("starts", c.starts - spk_base.starts);
	STAT("restarts", c.restarts - spk_base.restarts);

	STAT("ble_writes", rx.writes - rx_base.writes);
	STAT("ble_bytes", rx.bytes - rx_base.bytes);
	STAT("ble_rejected", rx.rejected - rx_base.rejected);
	STAT("ble_rejected_bytes", rx.rejected_bytes - rx_base.rejected_bytes);
	STAT("ring_bytes", rx.ring_level);
	STAT("ring_peak", rx.ring_peak);

	STAT("frames_decoded", c.frames_decoded - spk_base.frames_decoded);
	STAT("frames_plc", c.frames_plc - spk_base.frames_plc);
	STAT("frames_muted", c.frames_muted - spk_base.frames_muted);
	STAT("mute_events", c.mute_events - spk_base.mute_events);
	STAT("decode_errors", c.decode_errors - spk_base.decode_errors);
	STAT("bytes_misaligned", c.bytes_misaligned - spk_base.bytes_misaligned);
	STAT("frames_dropped_stop",
	     c.frames_dropped_stop - spk_base.frames_dropped_stop);
	STAT("frames_write_failed",
	     c.frames_write_failed - spk_base.frames_write_failed);
	STAT("pcm_bytes", c.pcm_bytes - spk_base.pcm_bytes);
	STAT("pcm_bytes_failed", c.pcm_bytes_failed - spk_base.pcm_bytes_failed);

#if defined(CONFIG_MAX98357A_AUDIO)
	struct max98357a_audio_tx_diag amp;

	max98357a_audio_tx_diag_get(&amp);
	STAT("blocks_played", amp.real_sends - amp_base.real_sends);
	STAT("silence_blocks", amp.silence_sends - amp_base.silence_sends);
	STAT("blocks_discarded", amp.stop_discards - amp_base.stop_discards);
	STAT("drain_timeouts", amp.drain_timeouts - amp_base.drain_timeouts);
	STAT("i2s_errors", (amp.err_completions - amp_base.err_completions) +
				   (amp.cb_send_fails - amp_base.cb_send_fails));
#endif

	if (reset) {
		spk_base = c;
		rx_base = rx;
		halo_ble_lua_audio_rx_peak_reset();
#if defined(CONFIG_MAX98357A_AUDIO)
		amp_base = amp;
#endif
	}

	return 1;
}

#undef STAT

void halo_lua_speaker_interrupt(void)
{
	/* Ask the pump loop to stop. It re-checks is_streaming under the mutex
	 * before every speaker write, so once we hold the mutex below it can
	 * no longer touch the speaker. */
	if (speaker_state.is_streaming) {
		speaker_state.is_streaming = false;

		/* Bounded wait: the pump may be parked in a BLE read with no
		 * incoming data and only notices the stop flag on its next
		 * iteration. It cannot write again either way, so proceed. */
		if (k_sem_take(&speaker_state.stream_exit_sem, K_MSEC(1500)) != 0) {
			LOG_WRN("Speaker stream did not drain in time");
		}
	}

	/* Release the speaker singleton so a system sound can acquire it. */
	k_mutex_lock(&speaker_state.mutex, K_FOREVER);
	if (speaker_state.speaker) {
		if (audio_speaker_check_owner(speaker_state.speaker, AUDIO_OWNER_LUA)) {
			audio_speaker_stop(speaker_state.speaker);
			audio_speaker_destroy(speaker_state.speaker);
		}
		speaker_state.speaker = NULL;
	}
	k_mutex_unlock(&speaker_state.mutex);
}

/**
 * @brief Cleanup function called when Lua closes
 */
static void lua_speaker_cleanup(void)
{
	/* Exit streaming loop first if active */
	if (speaker_state.is_streaming) {
		speaker_state.is_streaming = false;
		k_sem_take(&speaker_state.stream_exit_sem, K_FOREVER);
	}

	/* Then cleanup audio resources */
	speaker_cleanup_audio_resources();

	/* Finally stop thread */
	speaker_stop_thread();
}



/* ============================================================================
 * Service Lifecycle Management
 * ============================================================================ */
/**
 * @brief Service lifecycle event handler
 *
 * Handles lifecycle events like INIT, DEINIT, INTERRUPT, RESTART
 */
static int speaker_service_event_handler(halo_lua_event_t event, void *user_data)
{
	ARG_UNUSED(user_data);
	int ret = 0;

	switch (event) {
	case HALO_LUA_EVENT_INIT:
		break;

	case HALO_LUA_EVENT_DEINIT:
		/* Cleanup resources */
		lua_speaker_cleanup();
		break;

	case HALO_LUA_EVENT_SUSPEND:
		k_mutex_lock(&speaker_state.mutex, K_FOREVER);
		bool was_streaming = speaker_state.is_streaming;
		if (speaker_state.speaker) {
			audio_speaker_stop(speaker_state.speaker);
		}
		k_mutex_unlock(&speaker_state.mutex);
		/* Return 0 if was streaming (needs resume), 1 if not */
		ret = was_streaming ? 0 : 1;
		break;

	case HALO_LUA_EVENT_RESUME:
		k_mutex_lock(&speaker_state.mutex, K_FOREVER);
		if (speaker_state.speaker && speaker_state.is_streaming) {
			audio_speaker_start(speaker_state.speaker);
		}
		k_mutex_unlock(&speaker_state.mutex);
		break;

	case HALO_LUA_EVENT_INTERRUPT:
		/* Stop speaker if running */
		if (speaker_state.is_streaming) {
			lua_speaker_cleanup();
		}
		break;

	default:
		break;
	}

	return ret;
}

/* Register service with lifecycle management (power-managed service) */
HALO_LUA_SERVICE_DEFINE(speaker_service, speaker_service_event_handler, NULL, false);

/* ============================================================================
 * Library Registration
 * ============================================================================ */

/**
 * @brief Register speaker library to Lua
 *
 * Creates frame.speaker table with:
 *   - start(config) - Start/reconfigure speaker
 *   - stop() - Stop speaker
 *   - play(data) - Direct playback
 *   - volume(level) - Set volume
 *   - stats([reset]) - Playback accounting
 */
int lua_open_speaker_library(lua_State *L)
{

	/* Register service for lifecycle management */
	int ret = halo_lua_service_register(&speaker_service);
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("Failed to register speaker service: %d", ret);
		return ret;
	}

	/* Initialize state */
	memset(&speaker_state, 0, sizeof(speaker_state));
	k_mutex_init(&speaker_state.mutex);
	k_sem_init(&speaker_state.sem, 0, 1);
	k_sem_init(&speaker_state.stream_exit_sem, 0, 1);

	/* Get or create frame table */
	lua_getglobal(L, "frame");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setglobal(L, "frame");
	}

	/* Create speaker table */
	lua_newtable(L);

	/* Register functions */
	static const luaL_Reg speaker_funcs[] = {{"start", lua_speaker_start},
						 {"stop", lua_speaker_stop},
						 {"play", lua_speaker_play},
						 {"volume", lua_speaker_volume},
						 {"stats", lua_speaker_stats},
						 {NULL, NULL}};

	luaL_setfuncs(L, speaker_funcs, 0);

	/* Set frame.speaker */
	lua_setfield(L, -2, "speaker");

	/* Pop frame table */
	lua_pop(L, 1);

	LOG_DBG("Speaker library registered successfully");

	return 0;
}