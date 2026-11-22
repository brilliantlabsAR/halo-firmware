/*
 * Copyright (c) 2026 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_AUDIO_VOICE_BAND_H_
#define HALO_AUDIO_VOICE_BAND_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup audio_voice_band Voice-band filter for the echo canceller's output
 * @brief Band-pass the microphone to the band the echo canceller processes.
 *
 * The canceller adapts and suppresses within ~312-3800 Hz only (audio_aec.c:
 * below it lives the mic rumble, above it the bone-conduction echo is
 * incoherent distortion product that cannot be estimated). Outside that band
 * the microphone carries UNCANCELLED echo - measured on a Halo unit as ~71% of
 * the echo-leak energy above 3.8 kHz plus a sub-300 Hz tail - which sails
 * through at unity gain: it trips a server VAD on the Lua capture path and is
 * heard by the far end of a phone call as a thin, high-pitched echo. Confining
 * the microphone to the canceller's working band removes that echo while
 * keeping speech intelligible (telephone-band-plus).
 *
 * A cascaded 4th-order Butterworth high-pass (300 Hz) then low-pass (3400 Hz),
 * each realised as two 2nd-order RBJ biquad sections with the 4th-order
 * Butterworth Q pair, so the roll-off is steep enough to reject echo that hugs
 * the canceller's band edges (a 2nd-order band-pass left edge echo that still
 * tripped the VAD). Coefficients are computed at init; per-channel DF2T state,
 * up to two channels. Used after the canceller and the DC blocker, before
 * encoding, on both the Lua capture path and the LE Audio source path.
 * @{
 */

#ifndef AEC_VOICE_HP_HZ
#define AEC_VOICE_HP_HZ 300.0f
#endif
#ifndef AEC_VOICE_LP_HZ
#define AEC_VOICE_LP_HZ 3400.0f
#endif

#define AUDIO_VOICE_BAND_NSEC     2 /**< biquad sections per filter (4th order) */
#define AUDIO_VOICE_BAND_CHANNELS 2 /**< max interleaved channels */

/** @brief One a0-normalised biquad section */
struct audio_voice_biquad {
	float b0, b1, b2, a1, a2;
};

/** @brief Filter coefficients and per-channel state; one per audio path */
struct audio_voice_band {
	struct audio_voice_biquad hp[AUDIO_VOICE_BAND_NSEC];
	struct audio_voice_biquad lp[AUDIO_VOICE_BAND_NSEC];
	float hp_z1[AUDIO_VOICE_BAND_NSEC][AUDIO_VOICE_BAND_CHANNELS];
	float hp_z2[AUDIO_VOICE_BAND_NSEC][AUDIO_VOICE_BAND_CHANNELS];
	float lp_z1[AUDIO_VOICE_BAND_NSEC][AUDIO_VOICE_BAND_CHANNELS];
	float lp_z2[AUDIO_VOICE_BAND_NSEC][AUDIO_VOICE_BAND_CHANNELS];
};

/**
 * @brief Compute the coefficients for a sample rate and clear the state
 *
 * @param vb          Filter instance
 * @param sample_rate Microphone sample rate in Hz (> 0)
 */
void audio_voice_band_init(struct audio_voice_band *vb, int sample_rate);

/**
 * @brief Band-pass a block of interleaved int16 PCM in place
 *
 * Blocks with an unsupported channel count pass through untouched.
 *
 * @param vb       Filter instance
 * @param pcm      Interleaved int16 PCM, modified in place
 * @param samples  Total sample count (all channels)
 * @param channels Channel count (1 or 2)
 */
void audio_voice_band_process(struct audio_voice_band *vb, int16_t *pcm, size_t samples,
			      int channels);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* HALO_AUDIO_VOICE_BAND_H_ */
