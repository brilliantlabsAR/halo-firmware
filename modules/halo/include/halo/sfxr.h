/*
 * Copyright (c) 2026 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_SFXR_H_
#define HALO_SFXR_H_

#include <halo/audio_stream.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum halo_sfxr_waveform {
	HALO_SFXR_WAVEFORM_SQUARE = 0,
	HALO_SFXR_WAVEFORM_SAWTOOTH = 1,
	HALO_SFXR_WAVEFORM_SINE = 2,
	HALO_SFXR_WAVEFORM_NOISE = 3,
};

enum halo_sfxr_preset {
	HALO_SFXR_PRESET_RANDOM_PICKUP = 0,
	HALO_SFXR_PRESET_RANDOM_LASER,
	HALO_SFXR_PRESET_RANDOM_EXPLOSION,
	HALO_SFXR_PRESET_RANDOM_POWERUP,
	HALO_SFXR_PRESET_RANDOM_HIT,
	HALO_SFXR_PRESET_RANDOM_JUMP,
	HALO_SFXR_PRESET_RANDOM_BLIP,
};

struct halo_sfxr_params {
	uint32_t seed;
	uint8_t supersampling;
	enum halo_sfxr_waveform waveform;
	float repeatspeed;

	struct {
		float master;
		float sound;
	} volume;

	struct {
		float attack;
		float sustain;
		float punch;
		float decay;
	} envelope;

	struct {
		float start;
		float min;
		float slide;
		float dslide;
	} frequency;

	struct {
		float depth;
		float speed;
		float delay;
	} vibrato;

	struct {
		float amount;
		float speed;
	} change;

	struct {
		float ratio;
		float sweep;
	} duty;

	struct {
		float offset;
		float sweep;
	} phaser;

	struct {
		float cutoff;
		float sweep;
		float resonance;
	} lowpass;

	struct {
		float cutoff;
		float sweep;
	} highpass;
};

struct halo_sfxr_play_options {
	int sample_rate;
	int duration_ms;
	int volume;
	audio_owner_t owner;
};

void halo_sfxr_reset(struct halo_sfxr_params *params);
const char *halo_sfxr_preset_name(enum halo_sfxr_preset preset);
int halo_sfxr_preset_from_name(const char *name, enum halo_sfxr_preset *preset);
int halo_sfxr_make_preset(enum halo_sfxr_preset preset, uint32_t seed,
			  struct halo_sfxr_params *out);
int halo_sfxr_play(const struct halo_sfxr_params *params,
		   const struct halo_sfxr_play_options *options);
int halo_sfxr_play_named(const char *name, uint32_t seed,
			 const struct halo_sfxr_play_options *options);
int halo_sfxr_play_named_async(const char *name, uint32_t seed,
			       const struct halo_sfxr_play_options *options);
void halo_sfxr_stop_async(void);
bool halo_sfxr_is_async_playing(void);

/**
 * @brief Cancel Lua/app SFXR playback and block new non-system playback.
 *
 * Sets a quiesce latch: in-flight playback not owned by AUDIO_OWNER_SYSTEM is
 * cancelled at its next PCM chunk and new non-system playback is rejected with
 * -ECANCELED. System-owned sounds (startup/shutdown) still play. Used on the
 * way into deep sleep so no playback thread is holding the speaker or the
 * SOFT_OFF power-policy lock when the runtime is torn down.
 *
 * @param timeout_ms Max time to wait for in-flight playback to drain.
 * @return 0 once all playback has drained, -ETIMEDOUT otherwise.
 */
int halo_sfxr_quiesce(int timeout_ms);

/**
 * @brief Release the quiesce latch (deep-sleep entry failed and was aborted).
 */
void halo_sfxr_quiesce_cancel(void);

/**
 * @brief Play the configured shutdown sound synchronously.
 *
 * Retries briefly if the speaker is still busy. Compiled to a no-op returning
 * 0 when CONFIG_HALO_SHUTDOWN_SOUND is disabled.
 *
 * @return 0 if the sound played (or feature disabled), negative error otherwise.
 */
int halo_shutdown_sound_play(void);

void halo_startup_sound_schedule(void);

/**
 * @brief Block until the scheduled startup sound has finished (or failed).
 *
 * Returns when the startup sound thread has played the sound, given up waiting
 * for the speaker, or after @p timeout_ms elapses. Used to hold the boot splash
 * on screen for exactly as long as the sound plays.
 *
 * @param timeout_ms Max time to wait in ms, or negative to wait forever.
 * @return 0 if the sound completed, -EAGAIN on timeout.
 */
int halo_startup_sound_wait(int timeout_ms);

/**
 * @brief Mark this boot as settled, re-arming the boot/shutdown cues.
 *
 * Clears the persisted flag set just before a cue plays (see
 * CONFIG_HALO_CUE_GATE_DIRTY_BOOT): a boot that ends between cue start and
 * this call skips the cues on its next attempt. Call once late in
 * bring-up, after the MCUboot image confirm. No-op when the gate is
 * disabled.
 */
void halo_sound_cue_boot_settled(void);

#ifdef __cplusplus
}
#endif

#endif /* HALO_SFXR_H_ */
