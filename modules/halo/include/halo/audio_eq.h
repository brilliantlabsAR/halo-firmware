/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_AUDIO_EQ_H_
#define HALO_AUDIO_EQ_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup audio_eq Audio High-Pass Filter EQ
 * @brief Biquad IIR high-pass filter for bone conduction speaker protection.
 *
 * Bone conduction speakers require EQ to attenuate low-frequency energy which
 * causes excessive vibration amplitude, high instantaneous current draw (>700mA),
 * and potential battery protection chip shutdown.
 *
 * This implements a cascaded 2nd-order (biquad) Butterworth high-pass filter
 * using fixed-point Q30 arithmetic for int16 PCM processing on MCU.
 *
 * Note: on Halo this is disabled by default; speaker current protection is
 * owned by the MAX98357A driver chain (drivers/audio/max98357a/
 * speaker_protect.c), which runs post-volume. This module remains available
 * for output paths without driver-side protection.
 *
 * @{
 */

/** Maximum number of cascaded biquad stages */
#define AUDIO_HPF_MAX_STAGES 2

/**
 * @brief State for a single biquad section (Direct Form II Transposed)
 *
 * Uses Q30 fixed-point for coefficient storage and int64 for intermediate
 * accumulation to avoid overflow with int16 PCM input.
 */
struct audio_biquad_state {
	int64_t z1;  /**< First delay element */
	int64_t z2;  /**< Second delay element */
};

/**
 * @brief Biquad filter coefficients in Q30 fixed-point
 *
 * Normalized so a0 = 1.0 (coefficients are pre-divided by a0).
 * Q30 format: multiply float by (1 << 30) to get fixed-point value.
 */
struct audio_biquad_coeffs {
	int32_t b0;  /**< Numerator coefficient b0 (Q30) */
	int32_t b1;  /**< Numerator coefficient b1 (Q30) */
	int32_t b2;  /**< Numerator coefficient b2 (Q30) */
	int32_t a1;  /**< Denominator coefficient a1 (Q30) */
	int32_t a2;  /**< Denominator coefficient a2 (Q30) */
};

/**
 * @brief High-pass filter instance for one audio channel
 */
struct audio_hpf_channel {
	struct audio_biquad_state stages[AUDIO_HPF_MAX_STAGES];
};

/**
 * @brief High-pass filter context
 *
 * Supports up to 2 channels (stereo) with cascaded biquad stages.
 */
struct audio_hpf {
	struct audio_biquad_coeffs coeffs[AUDIO_HPF_MAX_STAGES];
	struct audio_hpf_channel channels[2];  /**< Per-channel filter state */
	int num_stages;                        /**< Number of cascaded stages (1 or 2) */
	int num_channels;                      /**< Number of channels (1=mono, 2=stereo) */
	bool enabled;                          /**< Filter enable flag */
};

/**
 * @brief Initialize high-pass filter
 *
 * Computes biquad coefficients for a Butterworth high-pass filter at the
 * specified cutoff frequency and sample rate.
 *
 * @param hpf       Filter context to initialize
 * @param sample_rate  Audio sample rate in Hz (e.g., 16000, 48000)
 * @param cutoff_hz    Cutoff frequency in Hz (e.g., 300)
 * @param num_channels Number of audio channels (1 or 2)
 * @param order        Filter order: 2 (single biquad, -12dB/oct) or
 *                     4 (cascaded biquads, -24dB/oct)
 * @return 0 on success, negative errno on error
 */
int audio_hpf_init(struct audio_hpf *hpf, int sample_rate, int cutoff_hz,
		   int num_channels, int order);

/**
 * @brief Reset filter state (clear delay elements)
 *
 * Call this when audio stream restarts to avoid transient artifacts.
 *
 * @param hpf Filter context
 */
void audio_hpf_reset(struct audio_hpf *hpf);

/**
 * @brief Apply high-pass filter to interleaved PCM buffer in-place
 *
 * Processes interleaved int16 PCM samples. For stereo, samples are
 * expected as [L0, R0, L1, R1, ...].
 *
 * @param hpf      Filter context (must be initialized)
 * @param pcm      Interleaved PCM buffer (modified in-place)
 * @param num_samples  Total number of samples in buffer
 *                     (frames * channels for interleaved)
 */
void audio_hpf_process(struct audio_hpf *hpf, int16_t *pcm, size_t num_samples);

/**
 * @brief Enable or disable the filter
 *
 * When disabled, audio_hpf_process() is a no-op.
 *
 * @param hpf     Filter context
 * @param enable  true to enable, false to disable
 */
void audio_hpf_set_enabled(struct audio_hpf *hpf, bool enable);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* HALO_AUDIO_EQ_H_ */
