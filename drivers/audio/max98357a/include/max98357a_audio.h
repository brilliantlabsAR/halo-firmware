#ifndef MAX98357A_AUDIO_H
#define MAX98357A_AUDIO_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MAX98357A audio trigger commands
 */
enum max98357a_audio_trigger_cmd {
	MAX98357A_AUDIO_TRIGGER_START,
	MAX98357A_AUDIO_TRIGGER_STOP,
	MAX98357A_AUDIO_TRIGGER_PAUSE,
	MAX98357A_AUDIO_TRIGGER_RESUME,
	MAX98357A_AUDIO_TRIGGER_DRAIN,
};

/**
 * @brief MAX98357A audio mode
 */
enum max98357a_audio_mode {
	MAX98357A_AUDIO_MONO = 0,
	MAX98357A_AUDIO_STEREO,
};

/**
 * @brief MAX98357A audio stream configuration
 */
struct max98357a_audio_stream_cfg {
	uint32_t sample_rate;
	uint8_t bits;
	enum max98357a_audio_mode mode;
	bool is_signed;
};

/**
 * @brief MAX98357A audio driver API structure
 */
struct max98357a_audio_driver_api {
	int (*configure)(const struct device *dev,
			 const struct max98357a_audio_stream_cfg *config);
	int (*set_volume)(const struct device *dev, uint8_t volume);
	int (*trigger)(const struct device *dev, enum max98357a_audio_trigger_cmd cmd);
	int (*write)(const struct device *dev, const void *data, size_t size,
		     size_t *bytes_written, k_timeout_t timeout);
};

/**
 * @brief TX tap: observe PCM blocks at the I2S send point.
 *
 * Called with each block as it is handed to the I2S controller - after
 * volume scaling and speaker-protection processing, i.e. the signal the
 * transducer actually emits, at most one DMA block before emission. May be
 * invoked from ISR context (the I2S TX-complete callback); implementations
 * must be ISR-safe and quick.
 *
 * @param pcm         Interleaved signed 16-bit PCM
 * @param samples     Total number of int16 samples (all channels)
 * @param sample_rate Stream sample rate in Hz
 * @param channels    Channel count (1 or 2)
 */
typedef void (*max98357a_audio_tx_tap_t)(const int16_t *pcm, size_t samples,
					 uint32_t sample_rate, uint8_t channels);

/**
 * @brief Register (or clear, with NULL) the TX tap.
 *
 * Single-slot: intended for the acoustic-echo-cancellation reference feed.
 */
void max98357a_audio_set_tx_tap(max98357a_audio_tx_tap_t tap);

/**
 * @brief TX-path diagnostic counters (AEC reference-feed accounting).
 *
 * Every mechanism by which the tap feed could diverge from real emission
 * is counted, so a reference-timeline slip can be attributed (or ruled
 * out) from live-session data instead of theory: error-status completions
 * (a completion whose block may not have fully emitted), tap-FIFO order
 * mismatches (feed dropped), and send failures inside the completion
 * callback (emission hole).
 */
struct max98357a_audio_tx_diag {
	uint32_t real_sends;      /**< audio blocks handed to I2S */
	uint32_t silence_sends;   /**< silence-feed blocks handed to I2S */
	uint32_t err_completions; /**< completions with status != OK */
	uint32_t tap_drops;       /**< tap-FIFO order-mismatch drops */
	uint32_t cb_send_fails;   /**< i2s_sync_send failures in the cb */
};

/** @brief Snapshot the TX diagnostic counters (racy; diagnostics only). */
void max98357a_audio_tx_diag_get(struct max98357a_audio_tx_diag *out);

/** @brief Zero the TX diagnostic counters. */
void max98357a_audio_tx_diag_zero(void);

/**
 * @brief Runtime overrides for the current-protection chain (bench tuning).
 *
 * Available when CONFIG_MAX98357A_AUDIO_PROTECT_TUNING is enabled. A tune
 * call REPLACES the whole override set: scalar fields at -1 (and unset
 * voicing/weights) fall back to their Kconfig/built-in defaults, so an
 * empty struct (scalars -1, flags false, pregain 0) restores stock
 * behaviour. Overrides take effect immediately when the stream is stopped,
 * otherwise at the next stream start.
 */
struct max98357a_protect_tuning {
	int hpf_hz;         /**< -1 = Kconfig default */
	int budget_percent; /**< -1 = Kconfig default */
	int env_ms;         /**< -1 = Kconfig default */
	int hold_ms;        /**< -1 = Kconfig default */
	int release_ms;     /**< -1 = Kconfig default */
	int ramp_ms;        /**< -1 = Kconfig default */
	int bass_percent;   /**< psychoacoustic bass drive; -1 = Kconfig default */
	int pregain_db10;   /**< digital pre-gain, dB*10; 0 = unity, max +120 */
	bool voicing_set;   /**< false = built-in voicing curve */
	int voicing_db10[6];/**< static voicing gains, dB*10 per band */
	bool weights_set;   /**< false = built-in current weights */
	int weights_x100[6];/**< limiter current weights * 100 per band */
};

/**
 * @brief Set the per-stream digital pre-gain (dB*10, clamped to 0..+120).
 *
 * Applied at the head of the protection chain; the limiter sidechain sees
 * the boosted signal, so the current budget still caps real transducer
 * drive (on hot sources the limiter rides the boost out - this is
 * compression, intended to lift quiet sources such as un-normalized TTS).
 *
 * Transient by convention: the halo audio_stream layer clears it to 0 on
 * every speaker acquisition, so it applies to one playback session only.
 * Composes additively with the bench-tuning override's pregain. No-op when
 * the protection chain is disabled.
 */
void max98357a_audio_set_stream_pregain(int gain_db10);

/** @brief Apply protection-chain overrides (see struct docs). */
int max98357a_audio_protect_tune(const struct device *dev,
				 const struct max98357a_protect_tuning *tuning);

/** @brief Read back the stored overrides, scalars resolved to effective values. */
int max98357a_audio_protect_get(const struct device *dev,
				struct max98357a_protect_tuning *out);

/**
 * @brief Read limiter activity since the last reset.
 *
 * @param min_gain_q15   lowest limiter gain seen (32768 = never limited)
 * @param frames         frames processed
 * @param limited_frames frames spent below unity gain
 * @param peak_out       largest |output sample|
 * @param reset          clear the counters after reading
 */
int max98357a_audio_protect_stats(const struct device *dev,
				  int32_t *min_gain_q15, uint32_t *frames,
				  uint32_t *limited_frames, int32_t *peak_out,
				  bool reset);

/**
 * @brief Configure MAX98357A audio stream
 *
 * @param dev MAX98357A device
 * @param config Stream configuration
 * @return 0 on success, negative error code on failure
 */
static inline int max98357a_audio_configure(const struct device *dev,
					    const struct max98357a_audio_stream_cfg *config)
{
	const struct max98357a_audio_driver_api *api =
		(const struct max98357a_audio_driver_api *)dev->api;

	return api->configure(dev, config);
}

/**
 * @brief Set MAX98357A audio volume
 *
 * @param dev MAX98357A device
 * @param volume Volume level (0-100)
 * @return 0 on success, negative error code on failure
 */
static inline int max98357a_audio_set_volume(const struct device *dev, uint8_t volume)
{
	const struct max98357a_audio_driver_api *api =
		(const struct max98357a_audio_driver_api *)dev->api;

	return api->set_volume(dev, volume);
}

/**
 * @brief Trigger MAX98357A audio operation
 *
 * @param dev MAX98357A device
 * @param cmd Trigger command
 * @return 0 on success, negative error code on failure
 */
static inline int max98357a_audio_trigger(const struct device *dev,
					  enum max98357a_audio_trigger_cmd cmd)
{
	const struct max98357a_audio_driver_api *api =
		(const struct max98357a_audio_driver_api *)dev->api;

	return api->trigger(dev, cmd);
}

/**
 * @brief Write audio data to MAX98357A
 *
 * @param dev MAX98357A device
 * @param data Audio data buffer
 * @param size Size of data to write
 * @param bytes_written Actual bytes written
 * @param timeout Write timeout
 * @return 0 on success, negative error code on failure
 */
static inline int max98357a_audio_write(const struct device *dev, const void *data,
					size_t size, size_t *bytes_written,
					k_timeout_t timeout)
{
	const struct max98357a_audio_driver_api *api =
		(const struct max98357a_audio_driver_api *)dev->api;

	return api->write(dev, data, size, bytes_written, timeout);
}

#ifdef __cplusplus
}
#endif

#endif /* MAX98357A_AUDIO_H */