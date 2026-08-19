/*
 * Copyright (c) 2026 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_AUDIO_AEC_H_
#define HALO_AUDIO_AEC_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup audio_aec Acoustic Echo Cancellation
 * @brief Adaptive cancellation of speaker leakage from the microphone path.
 *
 * The bone-conduction speaker couples strongly into the mics on the same
 * frame (echo path identified at +32..+39dB above the mic noise floor, with
 * 90% of the linear impulse response energy inside ~40ms). Duplex voice
 * applications re-send that echo as if the wearer had spoken, so barge-in
 * is unusable without cancellation.
 *
 * Pipeline position: strictly BEFORE any beamforming or encoding on the
 * microphone path (`mic -> AEC -> [beamform] -> LC3`).
 *
 * Reference signal: the far-end audio is tapped in the MAX98357A driver at
 * the I2S send point, post volume and post speaker-protection processing,
 * so the reference matches what the transducer actually emits and the bulk
 * delay is bounded by the I2S/DMIC block sizes (tens of ms, drift-bounded)
 * rather than the driver's DMA queue occupancy.
 *
 * Algorithm: per-bin partitioned-block FDAF (default; time-domain NLMS FIR
 * selectable via CONFIG_HALO_AUDIO_AEC_FDAF=n), float32 internally with
 * int16 PCM I/O, an error-power double-talk detector that freezes
 * adaptation while the wearer speaks, and a norm clamp as a divergence
 * guard. Linear cancellation is bounded (~13-16dB) by the measured harmonic
 * distortion of the speaker path; the FDAF build adds a per-bin residual
 * suppressor on the output path (predicted-echo-gated over-subtraction)
 * that covers both the nonlinear floor and the linear filter's adaptation
 * window at playback onset - the barge-in gap.
 *
 * v1 scope: mono microphone at 16kHz; the speaker reference may be mono or
 * stereo at 16kHz (the Halo I2S session runs stereo - one speaker per arm -
 * even for mono playback; a stereo reference is downmixed). Anything else
 * passes through untouched (logged once).
 *
 * @{
 */

/**
 * @brief Enable or disable echo cancellation at runtime.
 *
 * When disabled the microphone path is passed through untouched and the
 * reference feed is discarded. Exposed to Lua as frame.microphone.aec().
 */
void audio_aec_enable(bool enable);

/** @brief Whether echo cancellation is currently enabled. */
bool audio_aec_is_enabled(void);

/**
 * @brief Enable or disable the residual suppressor stage (FDAF build).
 *
 * The suppressor applies per-bin over-subtraction gains to the linear
 * canceller's output, gated by the predicted echo - it acts from the
 * first block of playback (no convergence) and mops up the nonlinear
 * residual the FIR cannot reach, at the cost of an 8ms group delay on
 * the mic path while playback is active. Enabled by default; the
 * toggle exists for on-device A/B of the linear stage alone. No-op in
 * the time-domain build.
 */
void audio_aec_suppress(bool enable);

/** @brief Whether the residual suppressor is enabled. */
bool audio_aec_is_suppressed(void);

/**
 * @brief Feed far-end reference PCM (what the speaker just emitted).
 *
 * ISR-safe: called from the speaker driver's DMA completion callback,
 * i.e. the block has just finished emitting - the feed is emission-exact
 * regardless of DMA queue occupancy. Single producer. Non-16kHz-mono
 * reference disables cancellation for the session (logged once).
 *
 * @param pcm         Interleaved int16 PCM as sent to the DAC
 * @param samples     Total sample count (all channels)
 * @param sample_rate Speaker sample rate in Hz
 * @param channels    Speaker channel count
 */
void audio_aec_feed_reference(const int16_t *pcm, size_t samples,
			      uint32_t sample_rate, uint8_t channels);

/**
 * @brief Cancel echo in a block of microphone PCM, in place.
 *
 * Called from the microphone consumer thread with each capture block,
 * before encoding/beamforming. Blocks are processed in arrival order;
 * the internal reference FIFO is consumed in lockstep and re-anchored
 * if it runs too far ahead or dry.
 *
 * @param pcm         Interleaved int16 microphone PCM, modified in place
 * @param samples     Total sample count (all channels)
 * @param sample_rate Microphone sample rate in Hz
 * @param channels    Microphone channel count
 */
void audio_aec_process(int16_t *pcm, size_t samples, uint32_t sample_rate,
		       uint8_t channels);

/**
 * @brief Account for capture samples lost upstream of the consumer.
 *
 * The reference window is anchored by reconstructing each mic block's
 * capture time from the cumulative sample count, so samples the driver
 * dropped (queue-full, allocation failure) silently retard the capture
 * timeline: the window then slips off the reference ring at the loss
 * rate and cancellation dies without a single resync being counted -
 * the 2026-07-13 live-duplex failure. Reporting the loss keeps the
 * timeline honest; a loss large enough to matter re-anchors through
 * the normal drift check (a counted resync), smaller ones are tracked
 * by the filter.
 *
 * Call from the mic consumer thread, before processing the block that
 * follows the loss.
 *
 * @param samples Lost sample count, per channel (mono samples)
 */
void audio_aec_note_mic_loss(size_t samples);

/** @brief Diagnostic snapshot of the canceller state. */
struct audio_aec_stats {
	float w_norm2;      /**< squared L2 norm of the filter. In the FDAF
			     *   build this is the ROUND-ROBIN shadow taps
			     *   (aec.w[]), which lag convergence by up to
			     *   FDAF_PARTS blocks and can read ~0 on short or
			     *   low-SNR runs - use fd_wnorm2 for the live
			     *   frequency-domain filter energy instead. */
	float sup_gmin;     /**< last block's minimum suppressor gain
			     *   (1.0 = not suppressing / disabled) */
	float fd_wnorm2;    /**< FDAF frequency-domain filter energy: sum|W|^2
			     *   over ALL partitions, computed at snapshot time
			     *   so it tracks convergence immediately (0 in the
			     *   time-domain build). */
	float sup_sy;       /**< total predicted-echo power the suppressor sees
			     *   (sum Sy over bins). ~0 => no echo to suppress,
			     *   so gmin stays 1.0 regardless of playback. */
	float sup_se;       /**< total residual power at the suppressor
			     *   (sum Se over bins). */
	float sup_gmean;    /**< mean suppressor gain across bins (1.0 = fully
			     *   open); gmin is the deepest single bin. */
	uint32_t sup_onset; /**< onset/resync duck hops remaining (0 = steady;
			     *   nonzero => the blanket onset ceiling is active,
			     *   the prime source of onset-window distortion). */
	float sup_gate_fast; /**< envelope-gate fast excess-residual level (0 in
			     *   the time-domain build or when the gate is off). */
	float sup_gate_floor;/**< envelope-gate slow ambient floor. fast >>
			     *   floor => near-end voice released the cap. */
	float sup_gate_mid;  /**< envelope-gate medium (~240ms) edge baseline. fast
			      *   >> mid => a voice-onset rising edge fired the
			      *   release (the fast near-end path). */
	uint32_t sup_gate_rel; /**< 1 = the envelope gate released the sustained
			     *   ceiling this block (near-end/idle), 0 = capped. */
	uint32_t sup_pb_hold; /**< 1 = the live reference-collapse guard held the
			     *   cap this block (real playback active but p_ref
			     *   collapsed to ~0) that PREF_MIN would have lifted.
			     *   Nonzero during a barge-flush feed-starvation. */
	float p_ref;        /**< smoothed high-passed reference power */
	float p_err;        /**< smoothed high-passed error power */
	float p_mic;        /**< smoothed high-passed mic power */
	uint32_t resyncs;      /**< reference FIFO re-anchors */
	uint32_t norm_clamps;  /**< divergence-guard rescales */
	uint32_t ref_underruns; /**< consume events that had to pad */
	uint64_t ref_pads;      /**< total padded samples while synced */
	uint32_t feed_gaps;     /**< emission gaps > 2 block periods */
	uint32_t feed_gap_ms;   /**< total duration of those gaps */

	/* Epoch-estimator refinements: each stream's epoch is a running
	 * minimum that is frozen shortly after (re)establishment - every
	 * later refinement would silently shift the reference window under
	 * the converged filter. "refines" were applied (establishment
	 * window); "late" ones were suppressed by the freeze. Magnitudes
	 * are cumulative milliseconds.
	 */
	uint32_t emit_refines;  /**< applied speaker-epoch refinements */
	uint32_t emit_refine_ms;
	uint32_t cap_refines;   /**< applied mic-epoch refinements */
	uint32_t cap_refine_ms;
	uint32_t emit_late;     /**< suppressed post-freeze (speaker) */
	uint32_t emit_late_ms;
	uint32_t cap_late;      /**< suppressed post-freeze (mic) */
	uint32_t cap_late_ms;

	/* Capture-timeline loss accounting: samples reported lost upstream
	 * (audio_aec_note_mic_loss), and the late-floor backstop for
	 * UNREPORTED losses - when the windowed-minimum lateness of the
	 * capture-epoch observations stays above threshold, the epoch is
	 * slid later by it (uncounted losses look exactly like a
	 * persistently-late timeline; genuine queue backlog returns to
	 * its floor within the window).
	 */
	uint64_t mic_lost;      /**< upstream losses reported, mono samples */
	uint32_t cap_slips;     /**< late-floor epoch slides applied */
	uint32_t cap_slip_ms;   /**< cumulative slide magnitude */
	int32_t cap_late_floor; /**< current windowed-min lateness, ms */
	uint32_t emit_slips;    /**< emit-side late-floor slides (unflagged
				 *   emission stretching, e.g. TX FIFO dry) */
	uint32_t emit_slip_ms;  /**< cumulative emit slide magnitude */

	/* Window placement: ring write index minus window end at consume
	 * time, in samples. Negative = the window's tail asked for
	 * not-yet-emitted samples (it padded) - an anchor sitting too new.
	 */
	int32_t margin_last;    /**< most recent consume's margin */
	int32_t margin_min;     /**< min since enable (INT32_MAX if none) */
	int32_t margin_max;     /**< max since enable (INT32_MIN if none) */
	int32_t ref_skew_adj;   /**< margin servo: accumulated samples the read
				 *   window is pulled earlier to cancel the emit-
				 *   vs-mic clock skew (0 unless AEC_MARGIN_SERVO) */

	/* Clock-rate probes for the PDM-vs-I2S skew hunt: mono frames and
	 * kernel-clock elapsed microseconds accumulated over contiguous
	 * activity only (gaps contribute neither frames nor time), per
	 * stream. frames / (us / 1e6) is the stream's effective sample rate
	 * against the crystal-referenced kernel timebase.
	 */
	uint64_t mic_frames;   /**< mic frames counted */
	uint64_t mic_us;       /**< active mic time in microseconds */
	uint32_t mic_runs;     /**< contiguous mic activity runs */
	uint64_t ref_frames;   /**< speaker reference frames counted */
	uint64_t ref_us;       /**< active reference time in microseconds */
	uint32_t ref_runs;     /**< contiguous reference activity runs */
};

/**
 * @brief Read diagnostic state (racy snapshot; diagnostics only).
 *
 * @param stats Filled with current stats (required)
 * @param taps  Receives the tap count
 * @return Pointer to the live tap array
 */
const float *audio_aec_snapshot(struct audio_aec_stats *stats, size_t *taps);

/** @brief Reset the clock-rate probes (start a fresh measurement window). */
void audio_aec_clkmon_zero(void);

/**
 * @brief Arm a one-shot paired capture.
 *
 * The next fully-served mic block processed during active playback is
 * snapshotted together with the raw reference window it was cancelled
 * against, so the true echo delay can be read by cross-correlation in
 * tap coordinates (directly comparable to the filter's peak tap).
 */
void audio_aec_pair_request(void);

/**
 * @brief Collect a completed paired capture.
 *
 * @param mic  Receives the raw mic block (int16)
 * @param ref  Receives the raw reference window (int16, taps + n samples;
 *             mic sample i aligns with ref[taps - 1 + i])
 * @param taps Receives the tap count
 * @return Mic block length n, or 0 if no capture has completed
 */
size_t audio_aec_pair_read(const int16_t **mic, const int16_t **ref,
			   size_t *taps);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* HALO_AUDIO_AEC_H_ */
