/*
 * Copyright (c) 2025  Brilliant Labs Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _PWM_AUDIO_H_
#define _PWM_AUDIO_H_

#include <stdint.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief pwm audio channel define
 *
 */
enum pwm_audio_mode {
	PWM_AUDIO_MONO = 0,   /*!< (mono)*/
	PWM_AUDIO_STEREO = 1, /*!< (stereo)*/
	PWM_AUDIO_MAX,
};

/**
 *  @brief pwm audio trigger define
 */
enum pwm_audio_trigger {
	PWM_AUDIO_TRIGGER_STOP,  /**< Stop stream */
	PWM_AUDIO_TRIGGER_START, /**< Start stream */
	PWM_AUDIO_TRIGGER_PAUSE, /**< Pause stream */
};

struct pwm_audio_stream_cfg {
	uint32_t sample_rate;
	uint8_t bits;
	bool is_signed;
	enum pwm_audio_mode mode;
};

/**
 * Function pointers for the pwm audio driver
 */
struct _pwm_audio_ops {
	int (*configure)(const struct device *dev, struct pwm_audio_stream_cfg *config);
	int (*set_vol)(const struct device *dev, uint8_t vol);
	int (*trigger)(const struct device *dev, enum pwm_audio_trigger cmd);
	int (*write)(const struct device *dev, const uint8_t *inbuf, size_t len,
		     size_t *bytes_written, int32_t timeout);
};

/**
 * @brief Configure the pwm audio
 *
 * @param dev pwm audio device
 * @param config pwm audio configuration
 *
 * @return 0 if successful, or a negative error code
 */
static inline int pwm_audio_configure(const struct device *dev, struct pwm_audio_stream_cfg *config)
{
	const struct _pwm_audio_ops *ops = (const struct _pwm_audio_ops *)dev->api;
	if (ops && ops->configure) {
		return ops->configure(dev, config);
	}
	return -ENOTSUP;
}

/**
 * @brief Trigger the pwm audio
 *
 * @param dev pwm audio device
 * @param cmd pwm audio trigger command
 *
 * @return 0 if successful, or a negative error code
 */
static inline int pwm_audio_trigger(const struct device *dev, enum pwm_audio_trigger cmd)
{
	const struct _pwm_audio_ops *ops = (const struct _pwm_audio_ops *)dev->api;
	if (ops && ops->trigger) {
		return ops->trigger(dev, cmd);
	}
	return -ENOTSUP;
}

/**
 * @brief Write audio data to the pwm audio
 *
 * @param dev pwm audio device
 * @param inbuf input buffer
 * @param len length of input buffer
 * @param bytes_written number of bytes written
 * @param timeout timeout in milliseconds
 *
 * @return 0 if successful, or a negative error code
 */

static inline int pwm_audio_write(const struct device *dev, const uint8_t *inbuf, size_t len,
				  size_t *bytes_written, int32_t timeout)
{
	const struct _pwm_audio_ops *ops = (const struct _pwm_audio_ops *)dev->api;
	if (ops && ops->write) {
		return ops->write(dev, inbuf, len, bytes_written, timeout);
	}
	return -ENOTSUP;
}

/**
 * @brief Set the volume of the pwm audio
 *
 * @param dev pwm audio device
 * @param vol volume
 *
 * @return 0 if successful, or a negative error code
 */
static inline int pwm_audio_set_volume(const struct device *dev, uint8_t vol)
{
	const struct _pwm_audio_ops *ops = (const struct _pwm_audio_ops *)dev->api;
	if (ops && ops->set_vol) {
		return ops->set_vol(dev, vol);
	}
	return -ENOTSUP;
}

#ifdef __cplusplus
}
#endif

#endif
