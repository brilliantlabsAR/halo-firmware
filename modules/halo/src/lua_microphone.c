/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/fs/fs.h>

#include "lua.h"
#include "lauxlib.h"

#include <halo/audio_stream.h>
#if defined(CONFIG_HALO_AUDIO_AEC)
#include <halo/audio_aec.h>
#include <max98357a_audio.h>
#endif
#include <halo/mem_manager.h>
#include <halo/lua_microphone.h>
#include <halo/lua_service.h>
#include <halo/lua_runtime.h>
#include <halo/pm_manager.h>
#include <halo/file_manager.h>
#include <halo/ble_manager.h>

LOG_MODULE_REGISTER(lua_microphone, CONFIG_HALO_LOG_LEVEL);

/* Forward declarations */
static void mic_thread_fn(void *arg1, void *arg2, void *arg3);
static void mic_cleanup_resources(void);
static uint32_t ring_buf_put_force(struct ring_buf *buf, const uint8_t *data, uint32_t size);
static void mic_ble_disconnect_work_handler(struct k_work *work);
static int mic_ble_event_handler(const struct halo_ble_event_data *event, void *user_data);

static struct halo_ble_callback mic_ble_cb;
static bool mic_ble_cb_registered;

static uint32_t mic_lua_start_ms;
static bool mic_first_ring_put_logged;
static bool mic_first_lua_read_logged;

/* ============================================================================
 * Microphone State Management
 * ============================================================================ */

/**
 * @brief Microphone state with integrated thread and resource management
 */
static struct {
	/* Audio hardware */
	audio_microphone_t *microphone;

/* LC3 encoders - one per channel for stereo support */
#define MAX_MIC_CHANNELS 2
	audio_codec_ctx_t *lc3_encoder[MAX_MIC_CHANNELS];

	/* Thread management */
	k_tid_t thread_tid;
	struct k_thread thread;
	K_THREAD_STACK_MEMBER(stack, CONFIG_HALO_LUA_MICROPHONE_TASK_STACK_SIZE);
	struct k_sem sem;
	struct k_sem thread_exit_sem;
	bool thread_should_exit;

	/* Configuration */
	bool use_lc3;
	bool is_streaming;
	bool voice_band;   /* voice mode: band-pass the AEC output to the AEC's
			    * working band (see mic_voice_bandpass) */
	int sample_rate;
	int channel_count;
	int bit_depth;     /* OUTPUT format only (PCM mode: 8 or 16). The capture
			    * pipeline always runs 16-bit; 8 means the thread
			    * downconverts after the processing stages. */
	int lc3_duration;
	int lc3_bitrate;

	/* Ring buffer for both PCM and LC3 modes */
	struct ring_buf ringbuf;
	uint8_t *ringbuf_data;
	struct k_mutex mutex;

	/* Error handling */
	bool error_pending;
	int last_error;
	struct k_work ble_disconnect_work;

	/* LC3 encoding buffers */
	uint8_t *lc3_buffer;
	size_t lc3_frame_size;
	size_t pcm_samples_per_frame;
	int16_t *channel_buffer; /* Buffer for single channel PCM data (de-interleaved) */

	/* AAD callback */
	int aad_callback_ref;
} mic_state = {
	.microphone = NULL,
	.lc3_encoder = {NULL, NULL},
	.thread_tid = NULL,
	.thread_should_exit = false,
	.use_lc3 = false,
	.is_streaming = false,
	.ringbuf_data = NULL,
	.lc3_buffer = NULL,
	.channel_buffer = NULL,
	.aad_callback_ref = LUA_NOREF,
	.error_pending = false,
	.last_error = 0,
};

/* ============================================================================
 * Ring Buffer Helper Functions
 * ============================================================================ */

/**
 * @brief Force write to ring buffer by discarding old data if necessary
 *
 * This function ensures all data is written to the ring buffer by automatically
 * discarding the oldest data when the buffer is full.
 *
 * @param buf Ring buffer pointer
 * @param data Data to write
 * @param size Size of data to write
 * @return Number of bytes written (always equals size)
 */
static uint32_t ring_buf_put_force(struct ring_buf *buf, const uint8_t *data, uint32_t size)
{
	uint32_t space = ring_buf_space_get(buf);

	/* If not enough space, discard old data to make room */
	if (space < size) {
		LOG_WRN("Ring buffer full, discarding old data (%u bytes needed, %u available)",
			size, space);
		/* Discard whole put-units only. All puts in a session are the same
		 * size, and the LC3 consumer parses fixed-size frames straight off
		 * the byte stream - a byte-granular discard would shift every frame
		 * boundary after the overflow and the rest of the stream would
		 * decode as noise. Dropping whole units loses time, not framing. */
		uint32_t to_discard = ROUND_UP(size - space, size);
		uint8_t dummy;

		for (uint32_t i = 0; i < to_discard; i++) {
			ring_buf_get(buf, &dummy, 1);
		}
	}

	/* Now write the data - should always succeed */
	return ring_buf_put(buf, data, size);
}

static void mic_log_first_ring_put(uint32_t bytes)
{
	if (!mic_first_ring_put_logged) {
		mic_first_ring_put_logged = true;
		LOG_DBG("Mic first ring put: %u bytes at +%u ms from Lua start",
			bytes, k_uptime_get_32() - mic_lua_start_ms);
	}
}

/* ============================================================================
 * Microphone Background Thread
 * ============================================================================ */

#if defined(CONFIG_HALO_AUDIO_AEC)
/* One-pole DC blocker on the AEC OUTPUT (post-subtraction, post-suppressor),
 * i.e. the near-end stream handed to LC3 and the server.
 *
 * The Halo PDM front-end bypasses its hardware DC-blocking IIR (board dts:
 * iir-bypass), leaving a large persistent DC bias (~1265 counts). The AEC
 * deliberately ignores it - adaptation is band-limited to the speech band and
 * the DTD/mic-power measures use high-passed views (see audio_aec.c) - so the
 * bias rides through the bit-faithful full-band output to Silero's VAD, wasting
 * LC3 headroom and forcing every client to DC-remove before measuring near-end
 * energy. Removing it HERE, downstream of every AEC stage, strips it without
 * perturbing the worn-validated cancellation or the mic-vs-reference alignment
 * (an upstream high-pass on the mic only - HW IIR or a pre-AEC filter - would).
 *
 *   y[n] = x[n] - x[n-1] + R * y[n-1]   R=0.997 -> ~7.6Hz corner @16kHz,
 * far below any voice content. Per-channel state, interleaved buffer.
 */
#define MIC_DCB_R 0.997f
static float mic_dcb_yprev[MAX_MIC_CHANNELS];
static int16_t mic_dcb_xprev[MAX_MIC_CHANNELS];
/* Warm-start latch: on the first block after a mic start, xprev is primed to
 * that block's first sample per channel so the blocker emits 0 for the initial
 * DC level and only tracks CHANGES from it. Starting from xprev=0 instead makes
 * the blocker differentiate the start-up DC step (the post-discard PDM
 * settling tail, ~700 counts - see the t5838 discard-duration-ms) into a
 * transient the voice band-pass then rings on: an audible start-up "tick" on
 * the AEC/voice feed even though the raw pop is already snipped. See
 * mic_dc_block. */
static bool mic_dcb_primed;

static void mic_dc_block_reset(void)
{
	for (int ch = 0; ch < MAX_MIC_CHANNELS; ch++) {
		mic_dcb_yprev[ch] = 0.0f;
		mic_dcb_xprev[ch] = 0;
	}
	mic_dcb_primed = false;
}

static void mic_dc_block(int16_t *buf, size_t num_samples, int channels)
{
	if (channels < 1 || channels > MAX_MIC_CHANNELS) {
		return;
	}
	/* Warm-start (see mic_dcb_primed): prime xprev to the first sample of
	 * each channel so y[0] = x[0] - xprev + R*yprev = 0 - the blocker begins
	 * in DC steady state instead of stepping up from zero. Runs once per mic
	 * start; interleaved, so the first `channels` samples are the first
	 * sample of each channel. */
	if (!mic_dcb_primed && num_samples >= (size_t)channels) {
		for (int c = 0; c < channels; c++) {
			mic_dcb_xprev[c] = buf[c];
		}
		mic_dcb_primed = true;
	}
	int ch = 0;
	for (size_t i = 0; i < num_samples; i++) {
		int16_t x = buf[i];
		float y = (float)x - (float)mic_dcb_xprev[ch] +
			  MIC_DCB_R * mic_dcb_yprev[ch];
		mic_dcb_xprev[ch] = x;
		mic_dcb_yprev[ch] = y;
		int32_t yi = (int32_t)(y >= 0.0f ? y + 0.5f : y - 0.5f);
		if (yi > INT16_MAX) {
			yi = INT16_MAX;
		} else if (yi < INT16_MIN) {
			yi = INT16_MIN;
		}
		buf[i] = (int16_t)yi;
		if (++ch >= channels) {
			ch = 0;
		}
	}
}

/* Voice mode: band-pass the AEC output to the band the AEC actually processes
 * (~312-3800Hz). The AEC only cancels (adaptation) and scrambles (the suppressor
 * cap) within that band; OUTSIDE it the mic carries UNCANCELLED echo - measured
 * on the Halo unit as ~71% of the echo-leak energy above 3.8kHz plus a sub-300Hz tail
 * - which sails through at unity gain and trips the server VAD (the assistant
 * self-interruption). Confining the mic to the AEC's working band removes that
 * echo while keeping speech intelligible for STT (telephone-band-plus). This
 * turns "AEC mode" into a "voice" mode; it is opt-in (mic start `voice=true`) so
 * a developer can still take the raw, full-band AEC'd feed.
 *
 * A cascaded 4th-order Butterworth high-pass (300Hz) then low-pass (3400Hz),
 * each realised as two 2nd-order RBJ biquad sections with the 4th-order
 * Butterworth Q pair, so the roll-off is steep enough to reject echo that hugs
 * the AEC's band edges (a 2nd-order band-pass left ~230-RMS edge echo that still
 * tripped the VAD). Coefficients computed at start; per-channel DF2T state.
 */
#ifndef AEC_VOICE_HP_HZ
#define AEC_VOICE_HP_HZ 300.0f
#endif
#ifndef AEC_VOICE_LP_HZ
#define AEC_VOICE_LP_HZ 3400.0f
#endif

/* 4th-order Butterworth = two 2nd-order sections with these section Q's */
#define MIC_VOICE_Q0   0.54119610f
#define MIC_VOICE_Q1   1.30656296f
#define MIC_VOICE_NSEC 2

struct mic_biquad { float b0, b1, b2, a1, a2; }; /* a0-normalized */
static struct mic_biquad voice_hp[MIC_VOICE_NSEC], voice_lp[MIC_VOICE_NSEC];
static float vhp_z1[MIC_VOICE_NSEC][MAX_MIC_CHANNELS];
static float vhp_z2[MIC_VOICE_NSEC][MAX_MIC_CHANNELS];
static float vlp_z1[MIC_VOICE_NSEC][MAX_MIC_CHANNELS];
static float vlp_z2[MIC_VOICE_NSEC][MAX_MIC_CHANNELS];

static void mic_biquad_lowpass(struct mic_biquad *bq, float fc, float fs, float q)
{
	float w0 = 6.2831853072f * fc / fs;   /* 2*pi*fc/fs */
	float c = cosf(w0), s = sinf(w0);
	float alpha = s / (2.0f * q);
	float a0 = 1.0f + alpha;

	bq->b0 = ((1.0f - c) * 0.5f) / a0;
	bq->b1 = (1.0f - c) / a0;
	bq->b2 = bq->b0;
	bq->a1 = (-2.0f * c) / a0;
	bq->a2 = (1.0f - alpha) / a0;
}

static void mic_biquad_highpass(struct mic_biquad *bq, float fc, float fs, float q)
{
	float w0 = 6.2831853072f * fc / fs;   /* 2*pi*fc/fs */
	float c = cosf(w0), s = sinf(w0);
	float alpha = s / (2.0f * q);
	float a0 = 1.0f + alpha;

	bq->b0 = ((1.0f + c) * 0.5f) / a0;
	bq->b1 = (-(1.0f + c)) / a0;
	bq->b2 = bq->b0;
	bq->a1 = (-2.0f * c) / a0;
	bq->a2 = (1.0f - alpha) / a0;
}

static inline float mic_biquad_step(const struct mic_biquad *bq, float x,
				    float *z1, float *z2)
{
	float y = bq->b0 * x + *z1;   /* Direct Form II transposed */

	*z1 = bq->b1 * x - bq->a1 * y + *z2;
	*z2 = bq->b2 * x - bq->a2 * y;
	return y;
}

static void mic_voice_init(int sample_rate)
{
	float fs = (float)sample_rate;
	static const float q[MIC_VOICE_NSEC] = { MIC_VOICE_Q0, MIC_VOICE_Q1 };

	for (int k = 0; k < MIC_VOICE_NSEC; k++) {
		mic_biquad_highpass(&voice_hp[k], AEC_VOICE_HP_HZ, fs, q[k]);
		mic_biquad_lowpass(&voice_lp[k], AEC_VOICE_LP_HZ, fs, q[k]);
		for (int ch = 0; ch < MAX_MIC_CHANNELS; ch++) {
			vhp_z1[k][ch] = vhp_z2[k][ch] = 0.0f;
			vlp_z1[k][ch] = vlp_z2[k][ch] = 0.0f;
		}
	}
}

static void mic_voice_bandpass(int16_t *buf, size_t num_samples, int channels)
{
	if (channels < 1 || channels > MAX_MIC_CHANNELS) {
		return;
	}
	int ch = 0;

	for (size_t i = 0; i < num_samples; i++) {
		float x = (float)buf[i];

		for (int k = 0; k < MIC_VOICE_NSEC; k++) {
			x = mic_biquad_step(&voice_hp[k], x,
					    &vhp_z1[k][ch], &vhp_z2[k][ch]);
		}
		for (int k = 0; k < MIC_VOICE_NSEC; k++) {
			x = mic_biquad_step(&voice_lp[k], x,
					    &vlp_z1[k][ch], &vlp_z2[k][ch]);
		}

		int32_t y = (int32_t)(x >= 0.0f ? x + 0.5f : x - 0.5f);

		if (y > INT16_MAX) {
			y = INT16_MAX;
		} else if (y < INT16_MIN) {
			y = INT16_MIN;
		}
		buf[i] = (int16_t)y;
		if (++ch >= channels) {
			ch = 0;
		}
	}
}
#endif

/**
 * @brief Microphone capture thread
 *
 * Continuously reads audio from DMIC and:
 * - In LC3 mode: encodes PCM to LC3 and stores in ring buffer
 * - In PCM mode: stores raw PCM in ring buffer
 * Both modes use ring buffer for Lua to read
 */
static void mic_thread_fn(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	/* Wait for start signal */
	k_sem_take(&mic_state.sem, K_FOREVER);

	while (!mic_state.thread_should_exit) {
		if (mic_state.thread_should_exit) {
			k_sleep(K_MSEC(20));
			continue;
		}

		void *buffer;
		size_t len;

		/* Read from DMIC (owner-checked: bail out if LE Audio preempted the mic) */
		int ret = audio_microphone_read_owned(mic_state.microphone, AUDIO_OWNER_LUA,
						      &buffer, &len);
		if (ret != 0) {
			if (ret == -EAGAIN) {
				if (mic_state.thread_should_exit) {
					break;
				}
				continue;
			}

			if ((ret == -ECANCELED || ret == -ENODEV) && mic_state.thread_should_exit) {
				break;
			}

			if (ret == -EPERM) {
				LOG_WRN("Lua microphone preempted by LE Audio, stopping");
			}

			mic_state.last_error = ret;
			mic_state.error_pending = true;
			mic_state.is_streaming = false;
			mic_state.thread_should_exit = true;
			continue;
		}

		if (!mic_state.is_streaming || mic_state.thread_should_exit) {
			audio_microphone_free_buffer_owned(mic_state.microphone, AUDIO_OWNER_LUA,
							   buffer);
			continue;
		}

		k_mutex_lock(&mic_state.mutex, K_FOREVER);

		/* Check again after acquiring mutex - state might have changed */
		bool should_process = mic_state.is_streaming && !mic_state.thread_should_exit;

		k_mutex_unlock(&mic_state.mutex);

		if (!should_process) {
			audio_microphone_free_buffer_owned(mic_state.microphone, AUDIO_OWNER_LUA,
							   buffer);
			continue;
		}

#if defined(CONFIG_HALO_AUDIO_AEC)
		/* Report upstream capture losses (PDM driver drops: queue
		 * full, allocation failure) to the AEC BEFORE this block:
		 * its reference window is anchored by cumulative sample
		 * count, and unaccounted losses slide the window off the
		 * reference ring at the loss rate (the 2026-07-13 duplex
		 * failure). The counter is ISR-written; snapshot torn-free.
		 */
		{
			extern uint64_t t5838_diag_dropped;
			extern uint64_t t5838_diag_lost_est;
			static uint64_t mic_drop_seen;
			static uint64_t mic_lost_est_seen;
			unsigned int key = irq_lock();
			uint64_t dropped = t5838_diag_dropped;
			uint64_t lost_est = t5838_diag_lost_est;

			irq_unlock(key);
			if (dropped < mic_drop_seen ||
			    lost_est < mic_lost_est_seen) {
				/* counters were zeroed (aec('zero')) */
				mic_drop_seen = dropped;
				mic_lost_est_seen = lost_est;
			}

			/* dropped counts all-channel samples; lost_est counts
			 * frames (= per-channel samples) already
			 */
			size_t loss =
				(size_t)(dropped - mic_drop_seen) /
					mic_state.channel_count +
				(size_t)(lost_est - mic_lost_est_seen);

			if (loss > 0) {
				audio_aec_note_mic_loss(loss);
			}
			mic_drop_seen = dropped;
			mic_lost_est_seen = lost_est;
		}

		/* Cancel speaker echo in place, before any encoding.
		 * (When beamforming lands it runs after this.)
		 */
		audio_aec_process((int16_t *)buffer, len / sizeof(int16_t),
				  mic_state.sample_rate, mic_state.channel_count);

		/* Strip the residual DC bias on the AEC output before encoding,
		 * downstream of every AEC stage (see mic_dc_block). */
		mic_dc_block((int16_t *)buffer, len / sizeof(int16_t),
			     mic_state.channel_count);

		/* Voice mode: band-pass to the AEC's working band so out-of-band
		 * echo can't reach the server VAD (see mic_voice_bandpass). */
		if (mic_state.voice_band) {
			mic_voice_bandpass((int16_t *)buffer,
					   len / sizeof(int16_t),
					   mic_state.channel_count);
		}
#endif

		if (mic_state.use_lc3) {
			/* LC3 encoding path - encode each channel separately */
			int16_t *pcm = (int16_t *)buffer;
			size_t num_samples = len / sizeof(int16_t);
			size_t samples_per_frame = mic_state.pcm_samples_per_frame;
			size_t frame_samples_total = samples_per_frame * mic_state.channel_count;

			/* Process frames */
			for (size_t i = 0; i < num_samples; i += frame_samples_total) {
				if (i + frame_samples_total > num_samples) {
					break; /* Incomplete frame */
				}

				/* Encode each channel separately (de-interleave and encode) */
				for (int ch = 0; ch < mic_state.channel_count; ch++) {
					/* De-interleave: extract single channel from LRLRLR...
					 * format */
					for (size_t j = 0; j < samples_per_frame; j++) {
						mic_state.channel_buffer[j] =
							pcm[i + ch + j * mic_state.channel_count];
					}

					/* Encode this channel using its dedicated encoder */
					uint8_t *ch_output = mic_state.lc3_buffer +
							     (ch * mic_state.lc3_frame_size);
					ret = audio_lc3_encode_frame(
						mic_state.lc3_encoder[ch], mic_state.channel_buffer,
						samples_per_frame * sizeof(int16_t), ch_output,
						mic_state.lc3_frame_size);

					if (ret != 0) {
						LOG_ERR("LC3 encode failed for channel %d: %d", ch,
							ret);
						mic_state.last_error = ret;
						mic_state.error_pending = true;
						mic_state.is_streaming = false;
						mic_state.thread_should_exit = true;
						break;
					}
				}

				if (mic_state.thread_should_exit) {
					break;
				}

				/* Store all LC3 frames in ring buffer: [L frame][R frame]... */
				size_t total_lc3_size =
					mic_state.lc3_frame_size * mic_state.channel_count;
				k_mutex_lock(&mic_state.mutex, K_FOREVER);
				uint32_t written = ring_buf_put_force(
					&mic_state.ringbuf, mic_state.lc3_buffer, total_lc3_size);
				k_mutex_unlock(&mic_state.mutex);

				if (written < total_lc3_size) {
					LOG_ERR("Failed to write LC3 frames: %u < %u", written,
						total_lc3_size);
				} else {
					mic_log_first_ring_put(written);
				}
			}
		} else {
			/* PCM mode - store in ring buffer */
			if (mic_state.bit_depth == 8) {
				/* Downconvert AFTER the 16-bit processing
				 * stages, using the PDM driver's own 8-bit
				 * convention: signed high byte, sample >> 8
				 * (t5838_alif_pdm.c). In place is safe — the
				 * int8 writes trail the int16 reads. */
				const int16_t *s16 = (const int16_t *)buffer;
				int8_t *s8 = (int8_t *)buffer;
				size_t n = len / sizeof(int16_t);

				for (size_t i = 0; i < n; i++) {
					s8[i] = (int8_t)(s16[i] >> 8);
				}
				len = n;
			}

			k_mutex_lock(&mic_state.mutex, K_FOREVER);
			uint32_t written = ring_buf_put_force(&mic_state.ringbuf, buffer, len);
			k_mutex_unlock(&mic_state.mutex);

			if (written < len) {
				LOG_ERR("Failed to write PCM data: %u < %u", written, len);
			} else {
				mic_log_first_ring_put(written);
			}
		}

		audio_microphone_free_buffer_owned(mic_state.microphone, AUDIO_OWNER_LUA, buffer);
	}

	k_sem_give(&mic_state.thread_exit_sem);
}

/* ============================================================================
 * Resource Cleanup
 * ============================================================================ */

static void mic_cleanup_resources(void)
{
	bool thread_running = mic_state.thread_tid != NULL;

	mic_state.is_streaming = false;

	if (thread_running) {
		mic_state.thread_should_exit = true;
		/* Wake the thread in case it's waiting on the semaphore */
		k_sem_give(&mic_state.sem);
	}

	if (mic_state.microphone &&
	    audio_microphone_check_owner(mic_state.microphone, AUDIO_OWNER_LUA)) {
		audio_microphone_stop(mic_state.microphone);
	}

	if (thread_running) {
		int exit_ret = k_sem_take(&mic_state.thread_exit_sem, K_MSEC(1000));
		if (exit_ret != 0) {
			LOG_WRN("Microphone thread did not exit in time (%d), aborting", exit_ret);
			k_thread_abort(mic_state.thread_tid);
		}

		int ret = k_thread_join(mic_state.thread_tid, K_MSEC(200));
		if (ret != 0) {
			LOG_WRN("Failed to reap microphone thread (%d)", ret);
		}

		mic_state.thread_tid = NULL;
		mic_state.thread_should_exit = false;
		k_sem_reset(&mic_state.sem);
		k_sem_reset(&mic_state.thread_exit_sem);
	}

	/* Reset state flags */
	mic_state.use_lc3 = false;
	mic_state.error_pending = false;
	mic_state.last_error = 0;

	/* Destroy microphone only if still owned by Lua */
	if (mic_state.microphone) {
		/* Check if still owned by Lua before destroying */
		if (audio_microphone_check_owner(mic_state.microphone, AUDIO_OWNER_LUA)) {
			audio_microphone_stop(mic_state.microphone);
			audio_microphone_destroy(mic_state.microphone);
		}
		/* Clear pointer regardless (it may have been preempted) */
		mic_state.microphone = NULL;
	}

	/* Destroy all LC3 encoders (Lua-owned resources) */
	for (int ch = 0; ch < MAX_MIC_CHANNELS; ch++) {
		if (mic_state.lc3_encoder[ch]) {
			audio_lc3_encoder_destroy(mic_state.lc3_encoder[ch]);
			mic_state.lc3_encoder[ch] = NULL;
		}
	}

	/* Free buffers (Lua-owned resources) */
	if (mic_state.ringbuf_data) {
		halo_free(mic_state.ringbuf_data);
		mic_state.ringbuf_data = NULL;
	}

	if (mic_state.lc3_buffer) {
		halo_free(mic_state.lc3_buffer);
		mic_state.lc3_buffer = NULL;
	}

	if (mic_state.channel_buffer) {
		halo_free(mic_state.channel_buffer);
		mic_state.channel_buffer = NULL;
	}
}

static void mic_ble_disconnect_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	k_mutex_lock(&mic_state.mutex, K_FOREVER);
	bool need_stop = mic_state.is_streaming || (mic_state.microphone != NULL);
	k_mutex_unlock(&mic_state.mutex);

	if (!need_stop) {
		return;
	}

	LOG_WRN("BLE disconnected while Lua microphone active, stopping microphone");
	mic_cleanup_resources();
}

static int mic_ble_event_handler(const struct halo_ble_event_data *event, void *user_data)
{
	ARG_UNUSED(user_data);

	if (!event) {
		return -EINVAL;
	}

	if (event->event == HALO_BLE_EVENT_DISCONNECTED) {
		k_work_submit(&mic_state.ble_disconnect_work);
	}

	return 0;
}

/* ============================================================================
 * Power Management Callbacks
 * ============================================================================ */

static void mic_aad_callback_wrapper(void)
{
	/* Wakeup system if sleeping */
	if (halo_pm_is_sleeping()) {
		halo_pm_wakeup(HALO_PM_WAKEUP_MICROPHONE);
	}

	if (mic_state.aad_callback_ref == LUA_NOREF) {
		return;
	}

	lua_State *L = halo_lua_get_state();
	if (!L || !halo_lua_is_running()) {
		return;
	}

	/* Get callback from registry */
	lua_rawgeti(L, LUA_REGISTRYINDEX, mic_state.aad_callback_ref);

	/* Call callback */
	if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
		const char *error = lua_tostring(L, -1);
		LOG_ERR("AAD callback error: %s", error);
		lua_pop(L, 1);
	}
}

/* ============================================================================
 * Lua API Implementation
 * ============================================================================ */

/**
 * @brief frame.microphone.start(config)
 */
static int lua_microphone_start(lua_State *L)
{
	if (!lua_istable(L, 1)) {
		return luaL_error(L, "Expected a configuration table");
	}

	LOG_DBG("Microphone start called");

	mic_state.thread_should_exit = false;

	/* Support reconfiguration: if already streaming, cleanup and restart with new parameters */
	if (mic_state.is_streaming) {
		mic_state.is_streaming = false;
		mic_cleanup_resources();
	}

	/* Parse configuration */
	const char *encoder = "pcm";
	int sample_rate = 8000;
	int bit_depth = 16;
	int channels = 1;
	int gain = 0;
	int lc3_duration = 1000;
	int lc3_bitrate = 16000;

	lua_getfield(L, 1, "encoder");
	if (lua_isstring(L, -1)) {
		encoder = lua_tostring(L, -1);
	}
	lua_pop(L, 1);

	mic_state.use_lc3 = (strcmp(encoder, "lc3") == 0);

	lua_getfield(L, 1, "sample_rate");
	if (lua_isnumber(L, -1)) {
		sample_rate = lua_tointeger(L, -1);
	}
	lua_pop(L, 1);

	if (sample_rate != 8000 && sample_rate != 16000) {
		return luaL_error(L, "Sample rate must be 8000 or 16000");
	}

	lua_getfield(L, 1, "bit_depth");
	if (lua_isnumber(L, -1)) {
		bit_depth = lua_tointeger(L, -1);
	}
	lua_pop(L, 1);

	if (bit_depth != 8 && bit_depth != 16) {
		return luaL_error(L, "Bit depth must be 8 or 16");
	}
	if (mic_state.use_lc3 && bit_depth != 16) {
		return luaL_error(L, "Bit depth applies to PCM only (LC3 encodes 16-bit input)");
	}

	lua_getfield(L, 1, "channels");
	if (lua_isnumber(L, -1)) {
		channels = lua_tointeger(L, -1);
	}
	lua_pop(L, 1);

	lua_getfield(L, 1, "gain");
	if (lua_isnumber(L, -1)) {
		gain = lua_tointeger(L, -1);
	}
	lua_pop(L, 1);

	if (gain < -10 || gain > 10) {
		return luaL_error(L, "Gain must be -10 to 10");
	}

	/* AEC and voice-band are two independent, opt-in post-processing stages
	 * on the mic path: raw mic -> [aec] -> [voice band-pass] -> encode. Both
	 * default off (absent field -> false), so start{} alone is raw capture.
	 * start{} is the declarative reset point for both; frame.microphone.aec()
	 * / .voice() toggle them live within a session. They are orthogonal:
	 * voice=true with aec=false simply band-passes the raw mic to the speech
	 * band (no echo removal). */
	bool voice_on = false;

	lua_getfield(L, 1, "voice");
	voice_on = lua_toboolean(L, -1);
	lua_pop(L, 1);

#if defined(CONFIG_HALO_AUDIO_AEC)
	bool aec_on = false;

	lua_getfield(L, 1, "aec");
	aec_on = lua_toboolean(L, -1);
	lua_pop(L, 1);

	audio_aec_enable(aec_on);

	mic_state.voice_band = voice_on;
	if (mic_state.voice_band) {
		mic_voice_init(sample_rate);
	}
#else
	mic_state.voice_band = voice_on;
#endif

	if (mic_state.use_lc3) {
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

	/* Save configuration */
	mic_state.sample_rate = sample_rate;
	mic_state.channel_count = channels;
	mic_state.bit_depth = bit_depth;
	mic_state.lc3_duration = lc3_duration;
	mic_state.lc3_bitrate = lc3_bitrate;
	mic_state.error_pending = false;
	mic_state.last_error = 0;

	/* Initialize microphone hardware — ALWAYS at 16-bit, whatever the
	 * requested output depth. The processing stages (AEC, DC-block, voice
	 * band-pass) are int16 throughout; running the driver at 8-bit fed
	 * them int8 pairs reinterpreted as int16 and destroyed every sample
	 * (#253). bit_depth=8 is now purely an output format: the capture
	 * thread downconverts after the pipeline. */
	mic_state.microphone = audio_microphone_init(sample_rate, 16, channels, gain,
						     AUDIO_OWNER_LUA);
	if (!mic_state.microphone) {
		return luaL_error(L, "Failed to initialize microphone (may be occupied by LE Audio)");
	}

	/* Initialize LC3 encoder if needed */
	if (mic_state.use_lc3) {
		/* Create one encoder per channel (LC3 is per-channel) */
		for (int ch = 0; ch < channels; ch++) {
			mic_state.lc3_encoder[ch] =
				audio_lc3_encoder_create(sample_rate, lc3_duration, lc3_bitrate);

			if (!mic_state.lc3_encoder[ch]) {
				mic_cleanup_resources();
				return luaL_error(L, "Failed to create LC3 encoder for channel %d",
						  ch);
			}
		}

		/* LC3 frame size is per channel */
		mic_state.lc3_frame_size =
			audio_lc3_get_frame_size(sample_rate, lc3_duration, lc3_bitrate);

		mic_state.pcm_samples_per_frame = (sample_rate * lc3_duration) / 100000;

		/* Allocate LC3 buffer for all channels (output is per-channel) */
		mic_state.lc3_buffer =
			halo_malloc(mic_state.lc3_frame_size * channels, HALO_MEM_REGION_AUTO);

		if (!mic_state.lc3_buffer) {
			mic_cleanup_resources();
			return luaL_error(L, "Failed to allocate LC3 buffer");
		}

		/* Allocate buffer for single channel de-interleaving */
		mic_state.channel_buffer = halo_malloc(
			mic_state.pcm_samples_per_frame * sizeof(int16_t), HALO_MEM_REGION_AUTO);

		if (!mic_state.channel_buffer) {
			mic_cleanup_resources();
			return luaL_error(L, "Failed to allocate channel buffer");
		}
	}
	/* Allocate ring buffer for PCM mode */
	mic_state.ringbuf_data =
		halo_malloc(CONFIG_HALO_LUA_MICROPHONE_RING_BUFFER_SIZE, HALO_MEM_REGION_EXTERNAL);

	if (!mic_state.ringbuf_data) {
		mic_cleanup_resources();
		return luaL_error(L, "Failed to allocate ring buffer");
	}

	ring_buf_init(&mic_state.ringbuf, CONFIG_HALO_LUA_MICROPHONE_RING_BUFFER_SIZE,
		      mic_state.ringbuf_data);

	/* Start background thread if not already running */
	if (!mic_state.thread_tid) {
		mic_state.thread_should_exit = false;
		k_sem_reset(&mic_state.thread_exit_sem);

		mic_state.thread_tid = k_thread_create(
			&mic_state.thread, mic_state.stack, K_THREAD_STACK_SIZEOF(mic_state.stack),
			mic_thread_fn, NULL, NULL, NULL, CONFIG_HALO_LUA_MICROPHONE_TASK_PRIORITY,
			0, K_NO_WAIT);

		k_thread_name_set(&mic_state.thread, "lua_mic");
	}

	/* Start microphone */
	int ret = audio_microphone_start(mic_state.microphone);
	if (ret != 0) {
		mic_cleanup_resources();
		return luaL_error(L, "Failed to start microphone: %d", ret);
	}

#if defined(CONFIG_HALO_AUDIO_AEC)
	mic_dc_block_reset();
#endif
	mic_state.is_streaming = true;
	mic_lua_start_ms = k_uptime_get_32();
	mic_first_ring_put_logged = false;
	mic_first_lua_read_logged = false;
	k_sem_give(&mic_state.sem);

	LOG_DBG("Mic Lua start at uptime %u ms", mic_lua_start_ms);
	LOG_DBG("Microphone started: %s %dHz %dbit gain=%ddB bitrate=%d",
		mic_state.use_lc3 ? "LC3" : "PCM", sample_rate, bit_depth, gain,
		mic_state.use_lc3 ? lc3_bitrate : 0);

	return 0;
}

/**
 * @brief frame.microphone.stop()
 */
static int lua_microphone_stop(lua_State *L)
{
	ARG_UNUSED(L);

	if (!mic_state.is_streaming) {
		return 0;
	}

	mic_state.is_streaming = false;
	mic_state.error_pending = false;
	mic_state.last_error = 0;

	mic_cleanup_resources();

	LOG_DBG("Microphone stopped");

	return 0;
}

/**
 * @brief frame.microphone.gain([value])
 * Get or set microphone gain
 * - No argument: returns current gain
 * - With argument: sets gain and returns nothing
 */
static int lua_microphone_gain(lua_State *L)
{
	/* Check if argument provided (setter) or not (getter) */
	if (lua_gettop(L) == 0) {
		/* Getter: return current gain */
		int gain = audio_microphone_get_gain(NULL);
		LOG_DBG("Microphone gain get: %d", gain);
		lua_pushinteger(L, gain);
		return 1;
	} else {
		/* Setter: set gain */
		if (!mic_state.microphone) {
			return luaL_error(L, "Microphone not initialized");
		}

		if (!audio_microphone_check_owner(mic_state.microphone, AUDIO_OWNER_LUA)) {
			return luaL_error(L, "Microphone in use by LE Audio");
		}

		int gain = luaL_checkinteger(L, 1);

		if (gain < -10 || gain > 10) {
			return luaL_error(L, "Gain must be -10 to 10");
		}

		k_mutex_lock(&mic_state.mutex, K_FOREVER);
		int ret = audio_microphone_set_gain(mic_state.microphone, gain);
		k_mutex_unlock(&mic_state.mutex);

		if (ret != 0) {
			return luaL_error(L, "Failed to set gain: %d", ret);
		}

		LOG_DBG("Microphone gain set: %d", gain);
		return 0;
	}
}

/**
 * @brief frame.microphone.read(bytes)
 *
 * Non-blocking FIFO read from the ring buffer (PCM and LC3). Returns nil if not
 * streaming, "" if nothing is available yet, or up to `bytes` of the oldest
 * buffered data (partial reads allowed; byte-accurate for LC3).
 */
static int lua_microphone_read(lua_State *L)
{
	size_t len = luaL_checkinteger(L, 1);
	if (mic_state.error_pending) {
		int err = mic_state.last_error;
		mic_state.error_pending = false;
		mic_cleanup_resources();
		if (err == -EPERM) {
			return luaL_error(L, "Microphone taken over by LE Audio");
		}
		return luaL_error(L, "Microphone read error: %d", err);
	}

	if (len == 0 || len % 2 != 0) {
		return luaL_error(L, "Bytes must be positive and even");
	}
	if (len > 4096) {
		return luaL_error(L, "Bytes exceeds maximum of 4096");
	}

	if (!mic_state.is_streaming) {
		lua_pushnil(L);
		return 1;
	}

	k_mutex_lock(&mic_state.mutex, K_FOREVER);
	uint32_t available = ring_buf_size_get(&mic_state.ringbuf);
	k_mutex_unlock(&mic_state.mutex);

	if (available == 0) {
		lua_pushstring(L, "");
		return 1;
	}

	size_t to_read = MIN((size_t)available, len);

	/* PCM only: keep 16-bit sample alignment. LC3: byte-accurate partial reads. */
	if (!mic_state.use_lc3) {
		to_read -= to_read % 2;
	}

	if (to_read == 0) {
		lua_pushstring(L, "");
		return 1;
	}

	uint8_t *buffer = halo_malloc(to_read, HALO_MEM_REGION_AUTO);
	if (!buffer) {
		return luaL_error(L, "Failed to allocate buffer");
	}

	k_mutex_lock(&mic_state.mutex, K_FOREVER);
	uint32_t read = ring_buf_get(&mic_state.ringbuf, buffer, to_read);
	k_mutex_unlock(&mic_state.mutex);

	if (!mic_first_lua_read_logged && read > 0) {
		mic_first_lua_read_logged = true;
		LOG_DBG("Mic first Lua read: %u bytes at +%u ms from Lua start",
			read, k_uptime_get_32() - mic_lua_start_ms);
	}

	lua_pushlstring(L, (const char *)buffer, read);
	halo_free(buffer);

	return 1;
}

/**
 * @brief frame.microphone.status()
 *
 * Returns the current microphone state:
 * - "le_audio":  the microphone is in use by a standard LE Audio (BAP) stream
 *                initiated by the connected host; frame.microphone.start() will
 *                fail until the host releases the stream
 * - "streaming": frame.microphone.start() is active and data is being captured
 * - "stopped":   the microphone is idle
 */
#if defined(CONFIG_HALO_AUDIO_AEC)
/* PDM driver diagnostic ledger (t5838_alif_pdm.c) for the duplex mic-rate
 * anomaly: what the ISR popped from the FIFO vs dropped on the way to the
 * rx queue.
 */
extern uint64_t t5838_diag_popped;
extern uint64_t t5838_diag_dropped;
extern uint64_t t5838_diag_discarded;
extern uint64_t t5838_diag_lost_est;
extern uint32_t t5838_diag_overflows;
extern uint32_t t5838_diag_isrs;
extern uint32_t t5838_diag_stat_max;
extern uint32_t t5838_diag_stat_gt8;
extern uint32_t t5838_diag_gap_max_us;
extern uint32_t t5838_diag_gap_over;

/**
 * @brief Lua: frame.microphone.aec([enable])
 *
 * Getter (no argument): returns whether echo cancellation is enabled.
 * Setter (boolean): enable/disable cancellation of speaker echo from the
 * microphone stream, live. Lets an app A/B the raw and cleaned mic feed.
 *
 * Voice-band post-filtering lives on frame.microphone.voice(); canceller
 * diagnostics live on frame.microphone.diag().
 */
static int lua_microphone_aec(lua_State *L)
{
	if (lua_gettop(L) == 0) {
		lua_pushboolean(L, audio_aec_is_enabled());
		return 1;
	}

	audio_aec_enable(lua_toboolean(L, 1));
	return 0;
}

/**
 * @brief Lua: frame.microphone.voice([enable])
 *
 * Getter (no argument): returns whether voice-band mode is enabled.
 * Setter (boolean): band-pass the AEC output to the AEC's working band
 * (~300-3400Hz), live, so out-of-band echo can't reach a server VAD (see
 * mic_voice_bandpass). Enabling recomputes the biquad coefficients for the
 * current sample rate and resets the filter state. Mirrors aec(): configure
 * initially with start{voice=true} or toggle here while streaming.
 */
static int lua_microphone_voice(lua_State *L)
{
	if (lua_gettop(L) == 0) {
		lua_pushboolean(L, mic_state.voice_band);
		return 1;
	}

	bool enable = lua_toboolean(L, 1);

	/* Recompute coefficients + reset state on the enabling edge, using the
	 * live sample rate. Skip when no stream has set the rate yet (a later
	 * start{} initialises it); guards mic_voice_init against fs == 0.
	 */
	if (enable && !mic_state.voice_band && mic_state.sample_rate > 0) {
		mic_voice_init(mic_state.sample_rate);
	}
	mic_state.voice_band = enable;
	return 0;
}

/**
 * @brief Lua: frame.microphone.diag(cmd)
 *
 * AEC canceller diagnostics, separate from the aec() control surface.
 *   diag('stats') -> table of canceller internals plus PDM/speaker/clock
 *                    probes (for the PDM-vs-I2S skew hunt)
 *   diag('zero')  -> zero the clock-rate monitor and PDM/speaker counters
 */
static int lua_microphone_diag(lua_State *L)
{
	const char *cmd = luaL_checkstring(L, 1);

	if (strcmp(cmd, "stats") == 0) {
		struct audio_aec_stats st;
		size_t taps;

		(void)audio_aec_snapshot(&st, &taps);

		/* Balletto clock configuration registers, for the
		 * PDM-vs-I2S skew hunt (decode against the HWRM):
		 * CGU OSC_CTRL / PLL_LOCK_CTRL / PLL_CLK_SEL /
		 * CLK_ENA, CLKCTL_PER_SLV I2S0_CTRL, M55HE
		 * HE_CLK_ENA, and the LPPDM config registers.
		 */
		static const struct {
			const char *name;
			uint32_t addr;
		} clk_regs[] = {
			{"r_osc_ctrl", 0x1A602000},
			{"r_pll_lock_ctrl", 0x1A602004},
			{"r_pll_clk_sel", 0x1A602008},
			{"r_cgu_clk_ena", 0x1A602014},
			{"r_i2s0_ctrl", 0x4902F010},
			{"r_he_clk_ena", 0x43007010},
			{"r_pdm_ctl0", 0x43002000},
			{"r_pdm_ctl1", 0x43002004},
		};

		lua_newtable(L);
		lua_pushnumber(L, st.w_norm2);
		lua_setfield(L, -2, "w_norm2");
		lua_pushnumber(L, st.sup_gmin);
		lua_setfield(L, -2, "sup_gmin");
		lua_pushnumber(L, st.fd_wnorm2);
		lua_setfield(L, -2, "fd_wnorm2");
		lua_pushnumber(L, st.sup_sy);
		lua_setfield(L, -2, "sup_sy");
		lua_pushnumber(L, st.sup_se);
		lua_setfield(L, -2, "sup_se");
		lua_pushnumber(L, st.sup_gmean);
		lua_setfield(L, -2, "sup_gmean");
		lua_pushinteger(L, st.sup_onset);
		lua_setfield(L, -2, "sup_onset");
		lua_pushnumber(L, st.sup_gate_fast);
		lua_setfield(L, -2, "sup_gate_fast");
		lua_pushnumber(L, st.sup_gate_floor);
		lua_setfield(L, -2, "sup_gate_floor");
		lua_pushnumber(L, st.sup_gate_mid);
		lua_setfield(L, -2, "sup_gate_mid");
		lua_pushinteger(L, st.sup_gate_rel);
		lua_setfield(L, -2, "sup_gate_rel");
		lua_pushinteger(L, st.sup_pb_hold);
		lua_setfield(L, -2, "sup_pb_hold");
		lua_pushnumber(L, st.p_ref);
		lua_setfield(L, -2, "p_ref");
		lua_pushnumber(L, st.p_err);
		lua_setfield(L, -2, "p_err");
		lua_pushnumber(L, st.p_mic);
		lua_setfield(L, -2, "p_mic");
		lua_pushinteger(L, st.resyncs);
		lua_setfield(L, -2, "resyncs");
		lua_pushinteger(L, st.norm_clamps);
		lua_setfield(L, -2, "norm_clamps");
		lua_pushinteger(L, st.ref_underruns);
		lua_setfield(L, -2, "ref_underruns");
		lua_pushnumber(L, (lua_Number)st.ref_pads);
		lua_setfield(L, -2, "ref_pads");
		lua_pushinteger(L, st.feed_gaps);
		lua_setfield(L, -2, "feed_gaps");
		lua_pushinteger(L, st.feed_gap_ms);
		lua_setfield(L, -2, "feed_gap_ms");
		lua_pushinteger(L, st.emit_refines);
		lua_setfield(L, -2, "emit_refines");
		lua_pushinteger(L, st.emit_refine_ms);
		lua_setfield(L, -2, "emit_refine_ms");
		lua_pushinteger(L, st.cap_refines);
		lua_setfield(L, -2, "cap_refines");
		lua_pushinteger(L, st.cap_refine_ms);
		lua_setfield(L, -2, "cap_refine_ms");
		lua_pushinteger(L, st.emit_late);
		lua_setfield(L, -2, "emit_late");
		lua_pushinteger(L, st.emit_late_ms);
		lua_setfield(L, -2, "emit_late_ms");
		lua_pushinteger(L, st.cap_late);
		lua_setfield(L, -2, "cap_late");
		lua_pushinteger(L, st.cap_late_ms);
		lua_setfield(L, -2, "cap_late_ms");
		lua_pushnumber(L, (lua_Number)st.mic_lost);
		lua_setfield(L, -2, "mic_lost");
		lua_pushinteger(L, st.cap_slips);
		lua_setfield(L, -2, "cap_slips");
		lua_pushinteger(L, st.cap_slip_ms);
		lua_setfield(L, -2, "cap_slip_ms");
		lua_pushinteger(L, st.cap_late_floor);
		lua_setfield(L, -2, "cap_late_floor");
		lua_pushinteger(L, st.emit_slips);
		lua_setfield(L, -2, "emit_slips");
		lua_pushinteger(L, st.emit_slip_ms);
		lua_setfield(L, -2, "emit_slip_ms");
		lua_pushinteger(L, st.margin_last);
		lua_setfield(L, -2, "margin_last");
		lua_pushinteger(L, st.ref_skew_adj);
		lua_setfield(L, -2, "ref_skew_adj");
		lua_pushinteger(L, st.margin_min);
		lua_setfield(L, -2, "margin_min");
		lua_pushinteger(L, st.margin_max);
		lua_setfield(L, -2, "margin_max");
		lua_pushnumber(L, (lua_Number)st.mic_frames);
		lua_setfield(L, -2, "mic_frames");
		lua_pushnumber(L, (lua_Number)st.mic_us);
		lua_setfield(L, -2, "mic_us");
		lua_pushinteger(L, st.mic_runs);
		lua_setfield(L, -2, "mic_runs");
		lua_pushnumber(L, (lua_Number)st.ref_frames);
		lua_setfield(L, -2, "ref_frames");
		lua_pushnumber(L, (lua_Number)st.ref_us);
		lua_setfield(L, -2, "ref_us");
		lua_pushinteger(L, st.ref_runs);
		lua_setfield(L, -2, "ref_runs");
		lua_pushnumber(L, (lua_Number)t5838_diag_popped);
		lua_setfield(L, -2, "pdm_popped");
		lua_pushnumber(L, (lua_Number)t5838_diag_dropped);
		lua_setfield(L, -2, "pdm_dropped");
		lua_pushnumber(L, (lua_Number)t5838_diag_discarded);
		lua_setfield(L, -2, "pdm_discarded");
		lua_pushinteger(L, t5838_diag_overflows);
		lua_setfield(L, -2, "pdm_overflows");
		lua_pushinteger(L, t5838_diag_isrs);
		lua_setfield(L, -2, "pdm_isrs");
		lua_pushinteger(L, t5838_diag_stat_max);
		lua_setfield(L, -2, "pdm_stat_max");
		lua_pushinteger(L, t5838_diag_stat_gt8);
		lua_setfield(L, -2, "pdm_stat_gt8");
		lua_pushinteger(L, t5838_diag_gap_max_us);
		lua_setfield(L, -2, "pdm_gap_max_us");
		lua_pushinteger(L, t5838_diag_gap_over);
		lua_setfield(L, -2, "pdm_gap_over");
		lua_pushnumber(L, (lua_Number)t5838_diag_lost_est);
		lua_setfield(L, -2, "pdm_lost_est");

		struct max98357a_audio_tx_diag spk;

		max98357a_audio_tx_diag_get(&spk);
		lua_pushinteger(L, spk.real_sends);
		lua_setfield(L, -2, "spk_real_sends");
		lua_pushinteger(L, spk.silence_sends);
		lua_setfield(L, -2, "spk_silence_sends");
		lua_pushinteger(L, spk.err_completions);
		lua_setfield(L, -2, "spk_err_completions");
		lua_pushinteger(L, spk.tap_drops);
		lua_setfield(L, -2, "spk_tap_drops");
		lua_pushinteger(L, spk.cb_send_fails);
		lua_setfield(L, -2, "spk_cb_send_fails");
		for (size_t i = 0; i < ARRAY_SIZE(clk_regs); i++) {
			lua_pushnumber(L,
				       (lua_Number)sys_read32(clk_regs[i].addr));
			lua_setfield(L, -2, clk_regs[i].name);
		}
		return 1;
	}

	if (strcmp(cmd, "zero") == 0) {
		audio_aec_clkmon_zero();
		t5838_diag_popped = 0;
		t5838_diag_dropped = 0;
		t5838_diag_discarded = 0;
		t5838_diag_overflows = 0;
		t5838_diag_isrs = 0;
		t5838_diag_stat_max = 0;
		t5838_diag_stat_gt8 = 0;
		t5838_diag_gap_max_us = 0;
		t5838_diag_gap_over = 0;
		t5838_diag_lost_est = 0;
		max98357a_audio_tx_diag_zero();
		return 0;
	}

	return luaL_error(L, "unknown diag command '%s'", cmd);
}
#endif

static int lua_microphone_status(lua_State *L)
{
	if (audio_microphone_get_owner() == AUDIO_OWNER_LE_AUDIO) {
		lua_pushstring(L, "le_audio");
	} else if (mic_state.is_streaming) {
		lua_pushstring(L, "streaming");
	} else {
		lua_pushstring(L, "stopped");
	}
	return 1;
}

/* ============================================================================
 * Service Lifecycle Management
 * ============================================================================ */
/**
 * @brief Service lifecycle event handler
 *
 * Handles lifecycle events like INIT, DEINIT, INTERRUPT, RESTART
 */
static int microphone_service_event_handler(halo_lua_event_t event, void *user_data)
{
	ARG_UNUSED(user_data);
	int ret = 0;
	switch (event) {
	case HALO_LUA_EVENT_INIT:
		break;

	case HALO_LUA_EVENT_DEINIT:
		/* Drop Lua callback ref only. Do not clear the C-level AAD wake path
		 * here — that breaks voice wake from standby/light sleep on the next
		 * frame.standby() cycle. Full disable is reserved for deep sleep. */
		mic_state.aad_callback_ref = LUA_NOREF;
		if (halo_pm_get_sleep_mode() == HALO_PM_SLEEP_DEEP) {
			LOG_DBG("Disabling AAD for deep sleep");
			audio_aad_disable();
		}
		/* Cleanup resources */
		mic_cleanup_resources();
		break;

	case HALO_LUA_EVENT_INTERRUPT:
		/* Stop microphone if running */
		if (mic_state.is_streaming) {
			mic_cleanup_resources();
		}
		break;

	case HALO_LUA_EVENT_SUSPEND:
		k_mutex_lock(&mic_state.mutex, K_FOREVER);

		bool was_streaming = mic_state.is_streaming;

		if (mic_state.microphone &&
		    audio_microphone_check_owner(mic_state.microphone, AUDIO_OWNER_LUA)) {
			audio_microphone_stop(mic_state.microphone);
		}

		/* Release DMIC singleton so T5838 can enter AAD mode before sleep. */
		if (mic_state.microphone &&
		    audio_microphone_check_owner(mic_state.microphone, AUDIO_OWNER_LUA)) {
			audio_microphone_destroy(mic_state.microphone);
			mic_state.microphone = NULL;
		}

		if (mic_state.ringbuf_data) {
			ring_buf_reset(&mic_state.ringbuf);
		}

		mic_state.is_streaming = false;

		k_mutex_unlock(&mic_state.mutex);

		halo_pm_sleep_mode_t mode = halo_pm_get_sleep_mode();
		if ((mode == HALO_PM_SLEEP_LIGHT || mode == HALO_PM_SLEEP_STANDBY) &&
		    mic_state.aad_callback_ref != LUA_NOREF) {
			int aad_ret = audio_aad_rearm();
			if (aad_ret < 0) {
				LOG_WRN("Failed to re-arm AAD for sleep mode %d: %d", mode,
					aad_ret);
			}
		}

		ret = was_streaming ? 0 : 1;
		break;

	case HALO_LUA_EVENT_RESUME:
		k_mutex_lock(&mic_state.mutex, K_FOREVER);

		/* Note: PM manager will only call this if we returned 0 from suspend */
		if (mic_state.microphone && mic_state.is_streaming) {
			audio_microphone_start(mic_state.microphone);
			k_sem_give(&mic_state.sem);
		}

		k_mutex_unlock(&mic_state.mutex);
		ret = 0;
		break;

	default:
		break;
	}

	return ret;
}

/* Register service with lifecycle management (power-managed service) */
HALO_LUA_SERVICE_DEFINE(microphone_service, microphone_service_event_handler, NULL, false);

/**
 * @brief frame.microphone.aad_callback(func, threshold, silent_period)
 *
 * Register acoustic activity detection callback with optional configuration.
 *
 * @param func Callback function or nil to clear
 * @param threshold Optional threshold in dB SPL (60-100). Defaults to 90 dB if not provided.
 *                  Hardware supports: 60, 65, 70, 75, 80, 85, 90, 95, 97.5 dB
 * @param silent_period Optional silent period in milliseconds (default: 1000 ms).
 *                      Time to wait before next detection after triggering.
 *
 * Examples:
 *   frame.microphone.aad_callback(my_func)           -- Use defaults (90 dB, 1000 ms)
 *   frame.microphone.aad_callback(my_func, 75)       -- 75 dB threshold, 1000 ms silent period
 *   frame.microphone.aad_callback(my_func, 80, 500)  -- 80 dB threshold, 500 ms silent period
 *   frame.microphone.aad_callback(nil)               -- Clear callback
 */
static int lua_microphone_aad_callback(lua_State *L)
{
	/* Unregister previous callback */
	if (mic_state.aad_callback_ref != LUA_NOREF) {
		luaL_unref(L, LUA_REGISTRYINDEX, mic_state.aad_callback_ref);
		mic_state.aad_callback_ref = LUA_NOREF;
	}
	int threshold_db = 90;
	int silent_period_ms = 1000;

	if (lua_isnil(L, 1)) {
		/* Clear callback */
		audio_aad_set_callback(NULL, 0, 0);
	} else if (lua_isfunction(L, 1)) {
		audio_aad_get_current_settings(&threshold_db, &silent_period_ms);
		/* Get optional threshold parameter (defaults to 90 dB) */
		if (lua_isnumber(L, 2)) {
			threshold_db = lua_tointeger(L, 2);
			if (threshold_db < 60 || threshold_db > 100) {
				return luaL_error(L, "Threshold must be between 60 and 100 dB");
			}
		}

		/* Get optional silent_period parameter (defaults to 1000 ms) */

		if (lua_isnumber(L, 3)) {
			silent_period_ms = lua_tointeger(L, 3);
			if (silent_period_ms < 0 || silent_period_ms > 10000) {
				return luaL_error(L,
						  "Silent period must be between 0 and 10000 ms");
			}
		}

		/* Register new callback */
		lua_pushvalue(L, 1);
		mic_state.aad_callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);

		audio_aad_set_callback(mic_aad_callback_wrapper, threshold_db, silent_period_ms);
		LOG_DBG("AAD callback registered: threshold=%d dB, silent_period=%d ms",
			threshold_db, silent_period_ms);
	} else {
		return luaL_error(L, "Expected function or nil");
	}

	return 0;
}

/* ============================================================================
 * Library Registration
 * ============================================================================ */

int lua_open_microphone_library(lua_State *L)
{

	/*register microphone service*/
	int ret = halo_lua_service_register(&microphone_service);
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("Failed to register bluetooth service: %d", ret);
		return ret;
	}

	/* Initialize state */
	memset(&mic_state, 0, sizeof(mic_state));
	mic_state.aad_callback_ref = LUA_NOREF;
	k_mutex_init(&mic_state.mutex);
	k_sem_init(&mic_state.sem, 0, 1);
	k_sem_init(&mic_state.thread_exit_sem, 0, 1);
	k_work_init(&mic_state.ble_disconnect_work, mic_ble_disconnect_work_handler);

	if (!mic_ble_cb_registered) {
		ret = halo_ble_register_callback(&mic_ble_cb, mic_ble_event_handler,
						 HALO_BLE_EVENT_MASK(HALO_BLE_EVENT_DISCONNECTED),
						 NULL, "lua_mic_ble", 10);
		if (ret < 0) {
			LOG_WRN("Failed to register BLE disconnect callback: %d", ret);
		} else {
			mic_ble_cb_registered = true;
		}
	}

	int threshold_db = 90;
	int silent_period_ms = 1000;
	audio_aad_get_current_settings(&threshold_db, &silent_period_ms);
	audio_aad_set_callback(mic_aad_callback_wrapper, threshold_db, silent_period_ms);

	/* Get or create frame table */
	lua_getglobal(L, "frame");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setglobal(L, "frame");
	}

	/* Create microphone table */
	lua_newtable(L);

	/* Register functions */
	static const luaL_Reg mic_funcs[] = {{"start", lua_microphone_start},
					     {"stop", lua_microphone_stop},
					     {"gain", lua_microphone_gain},
					     {"read", lua_microphone_read},
					     {"status", lua_microphone_status},
					     {"aad_callback", lua_microphone_aad_callback},
#if defined(CONFIG_HALO_AUDIO_AEC)
					     {"aec", lua_microphone_aec},
					     {"voice", lua_microphone_voice},
					     {"diag", lua_microphone_diag},
#endif
					     {NULL, NULL}};

	luaL_setfuncs(L, mic_funcs, 0);

	/* Set frame.microphone */
	lua_setfield(L, -2, "microphone");

	/* Pop frame table */
	lua_pop(L, 1);

	LOG_DBG("Microphone library registered");

	return 0;
}
