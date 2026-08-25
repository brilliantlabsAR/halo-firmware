/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file speaker_protect.h
 * @brief Bone-conduction speaker current-protection DSP chain.
 *
 * Self-contained fixed-point library (no Zephyr dependencies) so it can be
 * compiled and verified on a host machine (see tests/audio/speaker_protect).
 *
 * Chain (per sample, per channel; one shared gain computer per frame):
 *
 *   in (post-volume int16)
 *     -> onset ramp                  (spread inrush current at stream start)
 *     -> 2nd-order Butterworth HPF   (kill DC/subsonic below transducer
 *                                     resonance where current is pure waste)
 *     -> 6-band split                (cascaded one-pole low-passes; magnitude
 *                                     complementary, cheap, phase coherent)
 *     -> static voicing gains        (mild; roughly half the issue #180
 *                                     protection depths in dB - the energy
 *                                     limiter below now carries protection)
 *     -> current-proxy energy limiter:
 *          sidechain = sum over channels of |sum(band * current_weight)|,
 *          squared and leaky-integrated over ~ENV ms to approximate the
 *          battery-protection IC's over-current detection window.
 *          Gain: instant attack, HOLD ms hold, ~RELEASE ms exponential
 *          release. Low bands (< 425 Hz) receive the gain cubed (3x the dB
 *          cut) so reduction lands where the current is drawn and speech
 *          intelligibility (HF) is preserved.
 *     -> soft-knee clipper           (true-peak safety net, knee at 90 % FS)
 *
 * The limiter is feed-forward (sidechain taken before the gain is applied),
 * so it cannot limit-cycle and the computed gain exactly caps the weighted
 * output energy at the configured budget.
 *
 * Budget calibration: budget_percent is the amplitude, as a percentage of
 * int16 full scale, of a hypothetical weight-1.0 sine whose steady-state
 * weighted energy the limiter will allow. Every band carries a current
 * weight >= 2.0 (see current_weight_q12), so e.g. a >= 1.5 kHz sine
 * (weight 2.0) is allowed budget/2 of full scale sustained, and a 300 Hz
 * sine (weight 5.6) budget/5.6. Calibrate on the bench with full-scale
 * sine sweeps, BROADBAND NOISE, and real TTS at max volume in stereo:
 * find the largest budget that keeps battery current below the protection
 * IC's trip point with margin, and set the Kconfig default accordingly.
 * (Noise matters: the field failure that motivated the HF weight uplifts
 * was full-scale broadband garbage from a misconfigured LC3 decoder.)
 */

#ifndef SPEAKER_PROTECT_H_
#define SPEAKER_PROTECT_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Number of one-pole split points (bands = SPK_PROTECT_SPLITS + 1) */
#define SPK_PROTECT_SPLITS 5
#define SPK_PROTECT_BANDS  (SPK_PROTECT_SPLITS + 1)

#define SPK_PROTECT_MAX_CHANNELS 2

#define SPK_PROTECT_Q15_ONE 32768

/** Tuning parameters (typically sourced from Kconfig by the driver). */
struct spk_protect_params {
	int sample_rate;     /**< 8000..48000 Hz */
	int channels;        /**< 1 or 2 (interleaved) */
	int hpf_cutoff_hz;   /**< 2nd-order Butterworth HPF cutoff; 0 disables */
	int budget_percent;  /**< energy budget, % of full-scale reference-band
			      *   sine amplitude (see file header) */
	int env_ms;          /**< sidechain integration window, ~= battery IC
			      *   over-current detection delay */
	int hold_ms;         /**< limiter hold before release starts */
	int release_ms;      /**< limiter release time constant */
	int ramp_ms;         /**< onset ramp after reset; 0 disables */
	int bass_drive_percent; /**< psychoacoustic bass harmonic mix, percent
				 *   of the source-band level; 0 disables,
				 *   up to 200 */
};

/** Limiter activity counters; see spk_protect_stats_read(). */
struct spk_protect_stats {
	int32_t min_gain_q15;   /**< lowest limiter gain seen (Q15) */
	uint32_t frames;        /**< frames processed */
	uint32_t limited_frames;/**< frames with gain below unity */
	int32_t peak_out;       /**< largest |output sample| */
};

/** Per-channel filter state. */
struct spk_protect_channel {
	/* HPF biquad, Direct Form II Transposed, Q30 coeffs */
	int64_t hpf_z1;
	int64_t hpf_z2;
	/* One-pole low-pass states, Q15-extended sample domain */
	int32_t lp[SPK_PROTECT_SPLITS];
	/* Psychoacoustic bass enhancer states (Q15-extended) */
	int32_t bass_lp_lo;   /**< source band low edge (60 Hz LP) */
	int32_t bass_lp_hi;   /**< source band high edge (240 Hz LP, pole 1) */
	int32_t bass_lp_hi2;  /**< source band high edge (240 Hz LP, pole 2:
			       *   12 dB/oct keeps voice mids out of the
			       *   rectifier) */
	int32_t bass_lp_dc;   /**< DC/fundamental tracker on rectified band */
	int32_t bass_lp_out;  /**< harmonic top shaping (900 Hz LP) */
};

struct spk_protect {
	struct spk_protect_params params;

	/* HPF coefficients (Q30, a0-normalised) */
	int32_t hpf_b0, hpf_b1, hpf_b2, hpf_a1, hpf_a2;
	bool hpf_enabled;

	/* One-pole split coefficients (Q15) */
	int32_t alpha_q15[SPK_PROTECT_SPLITS];

	/* Per-instance voicing gains / current weights (defaults copied at
	 * init; overridable for bench tuning via the setters below).
	 */
	int32_t voicing_q15[SPK_PROTECT_BANDS];
	int32_t weight_q12[SPK_PROTECT_BANDS];

	/* Digital pre-gain applied before the HPF (Q15; may exceed unity up
	 * to 4x). The limiter sidechain sees the boosted signal, so the
	 * energy budget still caps real transducer drive.
	 */
	int32_t pregain_q15;

	struct spk_protect_stats stats;

	/* Psychoacoustic bass enhancer */
	bool bass_enabled;
	int32_t bass_drive_q15;   /**< harmonic mix gain */
	int32_t bass_alpha_lo;    /**< one-pole alphas (Q15) */
	int32_t bass_alpha_hi;
	int32_t bass_alpha_dc;
	int32_t bass_alpha_out;

	struct spk_protect_channel ch[SPK_PROTECT_MAX_CHANNELS];

	/* Limiter */
	int64_t env;            /**< leaky-integrated proxy energy */
	int32_t env_shift;      /**< integrator: env += (p2 - env) >> shift */
	int64_t threshold_env;  /**< energy budget in envelope units */
	int32_t gain_q15;       /**< current limiter gain (Q15) */
	int32_t hold_samples;   /**< configured hold length */
	int32_t hold_left;      /**< frames of hold remaining */
	int32_t release_shift;  /**< gain += (ONE - gain) >> shift per frame */

	/* Onset ramp */
	int32_t ramp_q15;       /**< 0..Q15_ONE */
	int32_t ramp_step;      /**< per-frame increment */
};

/**
 * @brief Initialise the protection chain.
 *
 * @return 0 on success, -1 on invalid parameters.
 */
int spk_protect_init(struct spk_protect *sp, const struct spk_protect_params *params);

/**
 * @brief Reset all state (filters, limiter, onset ramp).
 *
 * Call on stream (re)start: clears filter memory so no stale transient is
 * emitted, restarts the onset ramp to spread turn-on inrush current.
 */
void spk_protect_reset(struct spk_protect *sp);

/**
 * @brief Process interleaved int16 PCM in place.
 *
 * Samples are expected post-volume (absolute output level) so the energy
 * budget corresponds to real transducer drive. Buffers must contain whole
 * frames; a trailing partial frame is left untouched.
 *
 * @param num_samples total samples (frames * channels)
 */
void spk_protect_process(struct spk_protect *sp, int16_t *pcm, size_t num_samples);

/** @brief Current limiter gain (Q15); test/telemetry hook. */
static inline int32_t spk_protect_gain_q15(const struct spk_protect *sp)
{
	return sp->gain_q15;
}

/**
 * @brief Override the static voicing gains (Q15 per band).
 *
 * NULL restores the built-in defaults. Takes effect from the next sample.
 */
void spk_protect_set_voicing(struct spk_protect *sp,
			     const int32_t gains_q15[SPK_PROTECT_BANDS]);

/**
 * @brief Override the limiter sidechain current weights (Q12 per band).
 *
 * NULL restores the built-in defaults.
 */
void spk_protect_set_weights(struct spk_protect *sp,
			     const int32_t weights_q12[SPK_PROTECT_BANDS]);

/**
 * @brief Set the digital pre-gain (Q15; clamped to [0, 4 * Q15_ONE]).
 */
void spk_protect_set_pregain(struct spk_protect *sp, int32_t pregain_q15);

/** @brief dB*10 (e.g. -75 = -7.5 dB) to Q15 linear gain; saturates at 4x. */
int32_t spk_protect_db10_to_q15(int db10);

/**
 * @brief Read limiter activity counters accumulated since the last reset.
 *
 * @param reset clear the counters after reading
 */
void spk_protect_stats_read(struct spk_protect *sp,
			    struct spk_protect_stats *out, bool reset);

/** @brief Built-in default tables (Q15 gains / Q12 weights). */
const int32_t *spk_protect_default_voicing(void);
const int32_t *spk_protect_default_weights(void);

#ifdef __cplusplus
}
#endif

#endif /* SPEAKER_PROTECT_H_ */
