/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_AUDIO_STREAM_H_
#define HALO_AUDIO_STREAM_H_

#include <zephyr/kernel.h>
#include <zephyr/drivers/audio.h>
#include <zephyr/audio/dmic.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Resource Owner Management
 * ============================================================================ */

/**
 * @brief Audio resource owner types
 * 
 * Defines who currently owns the audio resources (speaker/microphone).
 * LE Audio has highest priority and can preempt other owners.
 */
typedef enum {
	AUDIO_OWNER_NONE = 0,      /* Resource not in use */
	AUDIO_OWNER_LUA,           /* Owned by Lua scripts */
	AUDIO_OWNER_SYSTEM,        /* Owned by firmware system features */
	AUDIO_OWNER_LE_AUDIO,      /* Owned by LE Audio (highest priority) */
} audio_owner_t;

/* ============================================================================
 * LC3 Codec API
 * ============================================================================ */

/**
 * @brief LC3 codec context (opaque type)
 */
typedef struct audio_codec_ctx audio_codec_ctx_t;

/**
 * @brief Create LC3 decoder
 *
 * @param sample_rate Sample rate (8000 or 16000 Hz)
 * @param duration Frame duration (750 or 1000 = 7.5ms or 10ms)
 * @param bitrate Bitrate (8000-96000, multiple of 8000)
 * @return Decoder context, or NULL on failure
 */
audio_codec_ctx_t *audio_lc3_decoder_create(int sample_rate, int duration, int bitrate);

/**
 * @brief Decode a single LC3 frame to PCM
 *
 * @param ctx Decoder context
 * @param encoded_data LC3 encoded data
 * @param encoded_len Encoded data length
 * @param pcm_out Output PCM buffer (int16_t samples)
 * @param pcm_len Output buffer size in bytes
 * @return 0 on success, negative errno on failure
 */
int audio_lc3_decode_frame(audio_codec_ctx_t *ctx,
			   const uint8_t *encoded_data, size_t encoded_len,
			   int16_t *pcm_out, size_t pcm_len);

/**
 * @brief Destroy LC3 decoder
 *
 * @param ctx Decoder context
 */
void audio_lc3_decoder_destroy(audio_codec_ctx_t *ctx);

/**
 * @brief Create LC3 encoder
 *
 * Creates a single-channel (mono) encoder. For stereo, create two separate encoder instances
 * in the application layer (e.g., lua_microphone).
 *
 * @param sample_rate Sample rate (8000 or 16000 Hz)
 * @param duration Frame duration (750 or 1000 = 7.5ms or 10ms)
 * @param bitrate Bitrate (8000-96000, multiple of 8000)
 * @return Encoder context, or NULL on failure
 */
audio_codec_ctx_t *audio_lc3_encoder_create(int sample_rate, int duration, int bitrate);

/**
 * @brief Encode a single channel PCM frame to LC3
 *
 * Encodes a single mono channel. For stereo, create multiple encoder contexts and call
 * this function separately for each channel (de-interleave in application layer).
 *
 * @param ctx Encoder context
 * @param pcm_data Input PCM data (int16_t samples, single channel)
 * @param pcm_len Input data length in bytes
 * @param encoded_out Output LC3 buffer
 * @param encoded_len Output buffer size
 * @return 0 on success, negative errno on failure
 */
int audio_lc3_encode_frame(audio_codec_ctx_t *ctx,
			   const int16_t *pcm_data, size_t pcm_len,
			   uint8_t *encoded_out, size_t encoded_len);

/**
 * @brief Destroy LC3 encoder
 *
 * @param ctx Encoder context
 */
void audio_lc3_encoder_destroy(audio_codec_ctx_t *ctx);

/**
 * @brief Get LC3 encoded frame size
 *
 * @param sample_rate Sample rate
 * @param duration Frame duration
 * @param bitrate Bitrate
 * @return Frame size in bytes
 */
size_t audio_lc3_get_frame_size(int sample_rate, int duration, int bitrate);

/**
 * @brief Get LC3 PCM frame size
 *
 * @param sample_rate Sample rate
 * @param duration Frame duration
 * @return PCM frame size in bytes
 */
size_t audio_lc3_get_pcm_frame_size(int sample_rate, int duration);

/* ============================================================================
 * Speaker Hardware API
 * ============================================================================ */

/**
 * @brief Speaker device handle (opaque type)
 */
typedef struct audio_speaker audio_speaker_t;

/**
 * @brief Initialize speaker device
 *
 * @param sample_rate Sample rate (Hz)
 * @param bit_depth Bit depth (8 or 16)
 * @param channels Number of channels (1=mono, 2=stereo)
 * @param owner Resource owner (AUDIO_OWNER_LUA or AUDIO_OWNER_LE_AUDIO)
 * @return Speaker handle, or NULL on failure (already owned by another)
 */
audio_speaker_t *audio_speaker_init(int sample_rate, int bit_depth, int channels,
				    audio_owner_t owner);

/**
 * @brief Start speaker playback
 *
 * @param spk Speaker handle
 * @return 0 on success, negative errno on failure
 */
int audio_speaker_start(audio_speaker_t *spk);

/**
 * @brief Write PCM data to speaker
 *
 * If CONFIG_HALO_AUDIO_HPF is enabled, a high-pass filter is applied
 * in-place before writing to hardware.
 *
 * @param spk Speaker handle
 * @param data PCM data buffer (may be modified in-place by HPF)
 * @param len Data length in bytes
 * @return Number of bytes written, or negative errno on failure
 */
int audio_speaker_write(audio_speaker_t *spk, uint8_t *data, size_t len);

/**
 * @brief Set speaker volume
 *
 * @param spk Speaker handle
 * @param volume Volume level (0-100)
 * @return 0 on success, negative errno on failure
 */
int audio_speaker_set_volume(audio_speaker_t *spk, int volume);

/**
 * @brief Set speaker volume without persisting it to settings
 *
 * @param spk Speaker handle
 * @param volume Volume level (0-100)
 * @return 0 on success, negative errno on failure
 */
int audio_speaker_set_volume_transient(audio_speaker_t *spk, int volume);

/**
 * @brief Set the per-stream digital pre-gain (dB*10, 0..+120)
 *
 * Boosts the signal into the protection chain's limiter: quiet sources
 * (e.g. un-normalized TTS) get louder, hot sources are compressed - the
 * current budget still caps real transducer drive. Transient: cleared to
 * unity on every audio_speaker_init(), so it applies to the current
 * playback session only.
 *
 * @param spk Speaker handle
 * @param gain_db10 Gain in dB*10 (e.g. 60 = +6 dB); 0 restores unity
 * @return 0 on success, negative errno on failure
 */
int audio_speaker_set_stream_gain(audio_speaker_t *spk, int gain_db10);

/**
 * @brief Set the per-stream energy-budget override (percent, 0 = none)
 *
 * Lets a stream choose its loudness/current envelope up to the
 * Kconfig-clamped maximum (default 100, the loudest bench-validated
 * point). Any override engages the fast limiter integration window,
 * which is what makes raised budgets safe against transient exposure.
 * All protection stays active (weights, limiter, true-peak clamp,
 * display budget drop). Transient: cleared on every
 * audio_speaker_init(). Must be set before the stream starts (it takes
 * effect when the protection chain is built).
 *
 * @param spk Speaker handle
 * @param budget_percent 10..100, or 0 to restore the configured default
 * @return 0 on success, negative errno on failure
 */
int audio_speaker_set_stream_budget(audio_speaker_t *spk, int budget_percent);

/**
 * @brief Tell the audio path whether the display is out of power save
 *
 * The display's supply current shares the battery protection IC with the
 * speakers, so the protection chain ducks its energy budget while the
 * display is active (a smooth ramped step, even mid-stream) and restores
 * it when the display sleeps. Called from the display power path; the
 * coupling is one-directional (display -> audio).
 *
 * @param active true when the display leaves power save, false on entry
 */
void audio_speaker_notify_display_active(bool active);

/**
 * @brief Get speaker volume
 *
 * @param spk Speaker handle
 * @return Current volume level (0-100), or negative errno on failure
 */
int audio_speaker_get_volume(audio_speaker_t *spk);

/**
 * @brief Stop speaker playback
 *
 * @param spk Speaker handle
 * @return 0 on success, negative errno on failure
 */
int audio_speaker_stop(audio_speaker_t *spk);

/**
 * @brief Destroy speaker device
 *
 * @param spk Speaker handle
 */
void audio_speaker_destroy(audio_speaker_t *spk);

/**
 * @brief Check if speaker is owned by expected owner
 *
 * @param spk Speaker handle
 * @param expected_owner Expected owner to check against
 * @return true if speaker is owned by expected_owner, false otherwise
 */
bool audio_speaker_check_owner(audio_speaker_t *spk, audio_owner_t expected_owner);

/* ============================================================================
 * Microphone Hardware API
 * ============================================================================ */

/**
 * @brief Microphone device handle (opaque type)
 */
typedef struct audio_microphone audio_microphone_t;

/**
 * @brief Initialize microphone device
 *
 * @param sample_rate Sample rate (Hz)
 * @param bit_depth Bit depth (8 or 16)
 * @param channels Number of channels (1=mono, 2=stereo)
 * @param gain Microphone gain (-10 to 10 dB)
 * @param owner Resource owner (AUDIO_OWNER_LUA or AUDIO_OWNER_LE_AUDIO)
 * @return Microphone handle, or NULL on failure (already owned by another)
 */
audio_microphone_t *audio_microphone_init(int sample_rate, int bit_depth, int channels,
					  int gain, audio_owner_t owner);

/**
 * @brief Start microphone capture
 *
 * @param mic Microphone handle
 * @return 0 on success, negative errno on failure
 */
int audio_microphone_start(audio_microphone_t *mic);

/**
 * @brief Read PCM data from microphone
 *
 * Reads a single block from DMIC. Caller must free buffer using
 * audio_microphone_free_buffer().
 *
 * @param mic Microphone handle
 * @param buffer Pointer to receive buffer address
 * @param len Pointer to receive buffer length
 * @return 0 on success, negative errno on failure
 */
int audio_microphone_read(audio_microphone_t *mic, void **buffer, size_t *len);

/**
 * @brief Free microphone buffer
 *
 * @param mic Microphone handle
 * @param buffer Buffer to free (from audio_microphone_read)
 */
void audio_microphone_free_buffer(audio_microphone_t *mic, void *buffer);

/**
 * @brief Set microphone gain
 *
 * @param mic Microphone handle
 * @param gain Gain in dB (-10 to 10)
 * @return 0 on success, negative errno on failure
 */
int audio_microphone_set_gain(audio_microphone_t *mic, int gain);

/**
 * @brief Get microphone gain
 *
 * @param mic Microphone handle
 * @return Current gain in dB (-10 to 10), or negative errno on failure
 */
int audio_microphone_get_gain(audio_microphone_t *mic);

/**
 * @brief Stop microphone capture
 *
 * @param mic Microphone handle
 * @return 0 on success, negative errno on failure
 */
int audio_microphone_stop(audio_microphone_t *mic);

/**
 * @brief Destroy microphone device
 *
 * @param mic Microphone handle
 */
void audio_microphone_destroy(audio_microphone_t *mic);

/**
 * @brief Check if microphone is owned by expected owner
 *
 * @param mic Microphone handle
 * @param expected_owner Expected owner to check against
 * @return true if microphone is owned by expected_owner, false otherwise
 */
bool audio_microphone_check_owner(audio_microphone_t *mic, audio_owner_t expected_owner);

/**
 * @brief Get the current owner of the microphone singleton
 *
 * @return Current owner, or AUDIO_OWNER_NONE if the microphone is not in use
 */
audio_owner_t audio_microphone_get_owner(void);

/**
 * @brief Read PCM data from microphone, verifying ownership first
 *
 * Same as audio_microphone_read() but returns -EPERM if the microphone is no
 * longer owned by @p owner (e.g. it was preempted by LE Audio). Use from
 * long-lived capture threads so a preempted owner backs off instead of
 * consuming blocks from the new owner's stream.
 *
 * @param mic Microphone handle
 * @param owner Owner this caller acquired the microphone as
 * @param buffer Pointer to receive buffer address
 * @param len Pointer to receive buffer length
 * @return 0 on success, -EPERM if ownership was lost, other negative errno on failure
 */
int audio_microphone_read_owned(audio_microphone_t *mic, audio_owner_t owner, void **buffer,
				size_t *len);

/**
 * @brief Free a microphone buffer, tolerating ownership loss
 *
 * Safe counterpart to audio_microphone_free_buffer() for preemptible owners:
 * if ownership was lost while a block was held, the block is returned to the
 * current slab only when it belongs to it, and silently dropped when it came
 * from a slab that has already been released (its backing memory was freed
 * wholesale, so nothing leaks).
 *
 * @param mic Microphone handle
 * @param owner Owner this caller acquired the microphone as
 * @param buffer Buffer to free (from audio_microphone_read/_read_owned)
 */
void audio_microphone_free_buffer_owned(audio_microphone_t *mic, audio_owner_t owner,
					void *buffer);

/* ============================================================================
 * AAD (Acoustic Activity Detection) API
 * ============================================================================ */

/**
 * @brief AAD callback function type
 */
typedef void (*audio_aad_callback_t)(void);


/**
 * @brief Get current AAD settings
 *
 * Retrieves the current threshold and silent period settings for AAD.
 *
 * @param threshold_db Pointer to receive threshold in dB SPL
 * @param silent_period_ms Pointer to receive silent period in milliseconds
 * @return 0 on success, negative errno on failure
 */
int audio_aad_get_current_settings(
	int *threshold_db,
	int *silent_period_ms
);

/**
 * @brief Set AAD callback with threshold and silent period configuration
 *
 * Configures AAD A mode with specified threshold and silent period. The function will 
 * automatically select the closest available hardware threshold from t5838_aad_a_thr enum.
 *
 * Available thresholds (dB SPL): 60, 65, 70, 75, 80, 85, 90, 95, 97.5
 *
 * @param callback Callback function to invoke on acoustic activity (NULL to disable)
 * @param threshold_db Desired threshold in dB SPL (60-100). Function will find closest match.
 *                     If 0 or negative, uses default (90 dB).
 * @param silent_period_ms Silent period in milliseconds before next detection.
 *                         If 0 or negative, uses default (1000 ms).
 * @return 0 on success, negative errno on failure
 */
int audio_aad_set_callback(audio_aad_callback_t callback, int threshold_db, int silent_period_ms);

/**
 * @brief Re-arm AAD using current callback and persisted settings
 *
 * Re-applies AAD mode and wake handler if a callback is currently registered.
 * Intended to be called before entering light sleep to ensure wake detection
 * remains armed across repeated sleep cycles.
 *
 * @return 0 on success, negative errno on failure
 */
int audio_aad_rearm(void);


/**
 * @brief Disable AAD
 *
 * Disables acoustic activity detection and clears any registered callback.
 *
 * @return 0 on success, negative errno on failure
 */
int audio_aad_disable(void);

/* ============================================================================
 * Audio Transport Callback API (for LE Audio / BLE Lua support)
 * ============================================================================ */

/**
 * @brief Audio source callback function type
 *
 * This callback is invoked when a speaker needs audio data from an external source
 * (e.g., BLE Lua service, LE Audio stack).
 *
 * @param data Buffer to fill with audio data
 * @param len Maximum number of bytes to read
 * @param timeout Timeout for read operation
 * @param user_data User data provided during registration
 * @return Number of bytes read, 0 on timeout, negative errno on error
 */
typedef size_t (*audio_source_callback_t)(uint8_t *data, size_t len,
					   k_timeout_t timeout, void *user_data);

/**
 * @brief Audio sink callback function type
 *
 * This callback is invoked when a microphone has audio data to send to an external sink
 * (e.g., BLE Lua service, LE Audio stack).
 *
 * @param data Audio data to send
 * @param len Number of bytes to send
 * @param user_data User data provided during registration
 * @return Number of bytes written, negative errno on error
 */
typedef size_t (*audio_sink_callback_t)(const uint8_t *data, size_t len, void *user_data);

/**
 * @brief Audio source handle (opaque type)
 */
typedef struct audio_source_handle *audio_source_handle_t;

/**
 * @brief Audio sink handle (opaque type)
 */
typedef struct audio_sink_handle *audio_sink_handle_t;

/**
 * @brief Register an audio source
 *
 * Registers a callback that provides audio data (e.g., from BLE or LE Audio).
 * Multiple sources can be registered with different priorities.
 *
 * @param callback Source callback function
 * @param user_data User data passed to callback
 * @param priority Priority (0 = highest, lower number = higher priority)
 * @param name Source name for debugging (optional)
 * @return Source handle, or NULL on failure
 */
audio_source_handle_t audio_stream_register_source(audio_source_callback_t callback,
						    void *user_data, int priority,
						    const char *name);

/**
 * @brief Unregister an audio source
 *
 * @param handle Source handle from audio_stream_register_source()
 * @return 0 on success, negative errno on failure
 */
int audio_stream_unregister_source(audio_source_handle_t handle);

/**
 * @brief Read audio data from registered sources
 *
 * Tries each registered source in priority order until data is available.
 * Used internally by lua_speaker.
 *
 * @param data Buffer to receive audio data
 * @param len Maximum number of bytes to read
 * @param timeout Timeout for read operation
 * @return Number of bytes read, 0 on timeout, negative errno on error
 */
size_t audio_stream_source_read(uint8_t *data, size_t len, k_timeout_t timeout);

/**
 * @brief Register an audio sink
 *
 * Registers a callback that receives audio data (e.g., to BLE or LE Audio).
 * Multiple sinks can be registered - audio is broadcast to all.
 *
 * @param callback Sink callback function
 * @param user_data User data passed to callback
 * @param priority Priority (0 = highest, lower number = higher priority)
 * @param name Sink name for debugging (optional)
 * @return Sink handle, or NULL on failure
 */
audio_sink_handle_t audio_stream_register_sink(audio_sink_callback_t callback, void *user_data,
						int priority, const char *name);

/**
 * @brief Unregister an audio sink
 *
 * @param handle Sink handle from audio_stream_register_sink()
 * @return 0 on success, negative errno on failure
 */
int audio_stream_unregister_sink(audio_sink_handle_t handle);

/**
 * @brief Write audio data to all registered sinks
 *
 * Broadcasts audio to all registered sinks in priority order.
 * Used internally by lua_microphone.
 *
 * @param data Audio data to send
 * @param len Number of bytes to send
 * @return Total number of bytes written across all sinks
 */
size_t audio_stream_sink_write(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* HALO_AUDIO_STREAM_H_ */
