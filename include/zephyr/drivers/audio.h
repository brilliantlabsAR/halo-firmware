#ifndef ZEPHYR_INCLUDE_DRIVERS_AUDIO_H_
#define ZEPHYR_INCLUDE_DRIVERS_AUDIO_H_

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Audio driver APIs
 * @defgroup audio_interface Audio Interface
 * @ingroup io_interfaces
 * @{
 */

/**
 * @brief Audio trigger commands
 */
enum audio_trigger {
	/** Start audio playback */
	AUDIO_TRIGGER_START,
	/** Stop audio playback */
	AUDIO_TRIGGER_STOP,
	/** Pause audio playback */
	AUDIO_TRIGGER_PAUSE,
	/** Resume audio playback */
	AUDIO_TRIGGER_RESUME,
	/** Drain remaining audio data */
	AUDIO_TRIGGER_DRAIN,
};

/**
 * @brief Audio channel modes
 */
enum audio_channel_mode {
	/** Mono audio */
	AUDIO_MONO = 1,
	/** Stereo audio */
	AUDIO_STEREO = 2,
};

/**
 * @brief Audio stream configuration
 */
struct audio_config {
	/** Sample rate in Hz */
	uint32_t sample_rate;
	/** Bits per sample (8, 16, 24, 32) */
	uint8_t bits_per_sample;
	/** Number of channels */
	enum audio_channel_mode channels;
	/** Whether samples are signed */
	bool is_signed;
};

/**
 * @brief Audio driver API structure
 */
__subsystem struct audio_driver_api {
	/**
	 * @brief Configure audio stream
	 *
	 * @param dev Audio device
	 * @param config Stream configuration
	 * @return 0 on success, negative error code on failure
	 */
	int (*configure)(const struct device *dev,
			 const struct audio_config *config);

	/**
	 * @brief Set audio volume
	 *
	 * @param dev Audio device
	 * @param volume Volume level (0-100)
	 * @return 0 on success, negative error code on failure
	 */
	int (*set_volume)(const struct device *dev, uint8_t volume);

	/**
	 * @brief Trigger audio operation
	 *
	 * @param dev Audio device
	 * @param cmd Trigger command
	 * @return 0 on success, negative error code on failure
	 */
	int (*trigger)(const struct device *dev, enum audio_trigger cmd);

	/**
	 * @brief Write audio data
	 *
	 * @param dev Audio device
	 * @param data Audio data buffer
	 * @param size Size of data to write
	 * @param bytes_written Actual bytes written (can be NULL)
	 * @param timeout Write timeout
	 * @return 0 on success, negative error code on failure
	 */
	int (*write)(const struct device *dev, const void *data, size_t size,
		     size_t *bytes_written, k_timeout_t timeout);

	/**
	 * @brief Read audio data (for input devices)
	 *
	 * @param dev Audio device
	 * @param data Buffer for audio data
	 * @param size Size of buffer
	 * @param bytes_read Actual bytes read (can be NULL)
	 * @param timeout Read timeout
	 * @return 0 on success, negative error code on failure
	 */
	int (*read)(const struct device *dev, void *data, size_t size,
		    size_t *bytes_read, k_timeout_t timeout);
};

/**
 * @brief Configure audio stream
 *
 * @param dev Audio device
 * @param config Stream configuration
 * @return 0 on success, negative error code on failure
 */
static inline int audio_configure(const struct device *dev,
				  const struct audio_config *config)
{
	const struct audio_driver_api *api =
		(const struct audio_driver_api *)dev->api;

	if (api->configure == NULL) {
		return -ENOSYS;
	}

	return api->configure(dev, config);
}

/**
 * @brief Set audio volume
 *
 * @param dev Audio device
 * @param volume Volume level (0-100)
 * @return 0 on success, negative error code on failure
 */
static inline int audio_set_volume(const struct device *dev, uint8_t volume)
{
	const struct audio_driver_api *api =
		(const struct audio_driver_api *)dev->api;

	if (api->set_volume == NULL) {
		return -ENOSYS;
	}

	return api->set_volume(dev, volume);
}

/**
 * @brief Trigger audio operation
 *
 * @param dev Audio device
 * @param cmd Trigger command
 * @return 0 on success, negative error code on failure
 */
static inline int audio_trigger(const struct device *dev, enum audio_trigger cmd)
{
	const struct audio_driver_api *api =
		(const struct audio_driver_api *)dev->api;

	if (api->trigger == NULL) {
		return -ENOSYS;
	}

	return api->trigger(dev, cmd);
}

/**
 * @brief Write audio data
 *
 * @param dev Audio device
 * @param data Audio data buffer
 * @param size Size of data to write
 * @param bytes_written Actual bytes written (can be NULL)
 * @param timeout Write timeout
 * @return 0 on success, negative error code on failure
 */
static inline int audio_write(const struct device *dev, const void *data,
			      size_t size, size_t *bytes_written,
			      k_timeout_t timeout)
{
	const struct audio_driver_api *api =
		(const struct audio_driver_api *)dev->api;

	if (api->write == NULL) {
		return -ENOSYS;
	}

	return api->write(dev, data, size, bytes_written, timeout);
}

/**
 * @brief Read audio data
 *
 * @param dev Audio device
 * @param data Buffer for audio data
 * @param size Size of buffer
 * @param bytes_read Actual bytes read (can be NULL)
 * @param timeout Read timeout
 * @return 0 on success, negative error code on failure
 */
static inline int audio_read(const struct device *dev, void *data, size_t size,
			     size_t *bytes_read, k_timeout_t timeout)
{
	const struct audio_driver_api *api =
		(const struct audio_driver_api *)dev->api;

	if (api->read == NULL) {
		return -ENOSYS;
	}

	return api->read(dev, data, size, bytes_read, timeout);
}

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_AUDIO_H_ */