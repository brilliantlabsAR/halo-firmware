/*
 * Modified from:
 *    ESPressif PWM Audio driver
 *
 * Copyright (c) 2025  Brilliant Labs Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/audio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>
#include <soc.h>
#include <pwm_audio.h>
#include "utimer.h"

#define DT_DRV_COMPAT alif_pwm_audio

LOG_MODULE_REGISTER(pwm_audio, CONFIG_PWM_AUDIO_LOG_LEVEL);

#define CHANNEL_LEFT_MASK  (0x01)
#define CHANNEL_RIGHT_MASK (0x02)
#define CHANNEL_DRIVER_A   (0x00)
#define CHANNEL_DRIVER_B   (0x01)
#define VOLUME_0DB         (100)
#define FADE_OUT_FACTORY   (0.99)
#define FADE_IN_MAX        (800)

struct pwm_audio_drv_data {
	uint32_t timer_base;
	uint32_t timer_left_base;
	uint32_t timer_right_base;
	uint32_t sample_rate;
	uint8_t bits;
	bool is_signed;
	bool is_fade_in;
	uint32_t fade_in_count;
	uint32_t fade_in_max;
	int8_t volume;
	enum pwm_audio_mode mode;
	uint8_t channel;
	uint8_t channel_state;
	struct ring_buf ring;
	struct k_sem ring_buf_sem;
	volatile uint32_t ring_buf_give;
	bool configured;
};

struct pwm_audio_drv_cfg {
	uint32_t regs;
	uint8_t resolution; /* pwm resulution */
	struct gpio_dt_spec power;
	int8_t timer;
	int8_t timer_left;
	int8_t timer_right;
	int8_t channel_left;
	int8_t channel_right;
	uint8_t *ring_buf;
	uint32_t ring_buf_size;
	void (*irq_config)(const struct device *dev);
	const struct pinctrl_dev_config *pcfg;
};

static inline void utimer_config_driver(uint32_t reg_base, uint8_t driver, uint32_t value)
{
	uint32_t temp;
	uint32_t mask = (COMPARE_CTRL_DRV_COMP_MATCH_Msk | COMPARE_CTRL_DRV_CYCLE_END_Msk);

	if (driver == CHANNEL_DRIVER_A) {
		temp = sys_read32(UTIMER_COMPARE_CTRL_A(reg_base));
		temp &= ~mask;
		temp |= value;
		sys_write32(temp, UTIMER_COMPARE_CTRL_A(reg_base));
	} else {
		temp = sys_read32(UTIMER_COMPARE_CTRL_B(reg_base));
		temp &= ~mask;
		temp |= value;
		sys_write32(temp, UTIMER_COMPARE_CTRL_B(reg_base));
	}

	sys_write32(0x33000004, UTIMER_BUF_OP_CTRL(reg_base));
}

static inline void utimer_enable_output(uint32_t reg_base, uint8_t driver, uint8_t timer)
{
	if (driver == CHANNEL_DRIVER_A) {
		sys_clear_bit(UTIMER_GLB_DRIVER_OEN(reg_base), (timer * 2));
	} else {
		sys_clear_bit(UTIMER_GLB_DRIVER_OEN(reg_base), ((timer * 2) + 1));
	}
}

static inline void utimer_compare_disable_driver(uint32_t reg_base, uint8_t driver)
{
	if (driver == CHANNEL_DRIVER_A) {
		sys_clear_bit(UTIMER_COMPARE_CTRL_A(reg_base), COMPARE_CTRL_DRV_DRIVER_EN_BIT);
	} else {
		sys_clear_bit(UTIMER_COMPARE_CTRL_B(reg_base), COMPARE_CTRL_DRV_DRIVER_EN_BIT);
	}
}

static inline void utimer_compare_enable_driver(uint32_t reg_base, uint8_t driver)
{
	if (driver == CHANNEL_DRIVER_A) {
		sys_set_bit(UTIMER_COMPARE_CTRL_A(reg_base), COMPARE_CTRL_DRV_DRIVER_EN_BIT);
		sys_clear_bit(UTIMER_COMPARE_CTRL_A(reg_base),
			      COMPARE_CTRL_DRV_DISABLE_VAL_HIGH_BIT);
	} else {
		sys_set_bit(UTIMER_COMPARE_CTRL_B(reg_base), COMPARE_CTRL_DRV_DRIVER_EN_BIT);
		sys_clear_bit(UTIMER_COMPARE_CTRL_B(reg_base),
			      COMPARE_CTRL_DRV_DISABLE_VAL_HIGH_BIT);
	}
}

static void utimer_update_pulse(uint32_t reg_base, uint8_t driver, uint32_t pulse)
{
	uint32_t reg = (driver == CHANNEL_DRIVER_A) ? UTIMER_COMPARE_A_BUF1(reg_base)
						    : UTIMER_COMPARE_B_BUF1(reg_base);

	// Clear counter if pulse decreases
	// if (pulse < sys_read32(reg)) {
	// 	sys_write32(0, UTIMER_CNTR(reg_base));
	// }

	sys_write32(pulse, reg);
}

static void timer_isr(const struct device *dev)
{
	const struct pwm_audio_drv_cfg *drv_cfg = dev->config;
	struct pwm_audio_drv_data *drv_data = dev->data;
	alif_utimer_clear_interrupt(drv_data->timer_base, CHAN_INTERRUPT_OVER_FLOW_BIT);
	static uint8_t wave_h, wave_l;
	static uint16_t value, value_old;
	static uint16_t value_l, value_r, value_l_o, value_r_old;
	struct ring_buf *ring = &drv_data->ring;

	if (drv_data->mode == PWM_AUDIO_MONO) {
		if (drv_cfg->resolution > 8) {
			if (ring_buf_size_get(ring) >= 2) {
				ring_buf_get(ring, &wave_l, 1);
				ring_buf_get(ring, &wave_h, 1);
				value = ((wave_h << 8) | wave_l);
			} else {
				// fade out
				value = value_old * FADE_OUT_FACTORY;
			}
		} else {
			if (ring_buf_size_get(ring) >= 1) {
				ring_buf_get(ring, &wave_l, 1);
				value = wave_l;
			} else {
				// fade out
				value = value_old * FADE_OUT_FACTORY;
			}
		}
		// don't update if value is the same as the previous one
		if (value != value_old) {
			value_old = value;
			if (value == 0) {
				if (drv_data->channel & CHANNEL_LEFT_MASK) {
					if (drv_data->channel_state & CHANNEL_LEFT_MASK) {
						drv_data->channel_state &= ~CHANNEL_LEFT_MASK;
						alif_utimer_set_counter_value(
							drv_data->timer_left_base, 0);
						utimer_compare_disable_driver(
							drv_data->timer_left_base,
							drv_cfg->channel_left);
					}
				}
				if (drv_data->channel & CHANNEL_RIGHT_MASK) {
					if (drv_data->channel_state & CHANNEL_RIGHT_MASK) {
						drv_data->channel_state &= ~CHANNEL_RIGHT_MASK;
						alif_utimer_set_counter_value(
							drv_data->timer_right_base, 0);
						utimer_compare_disable_driver(
							drv_data->timer_right_base,
							drv_cfg->channel_right);
					}
				}
			} else {
				// apply audio data to echo channel
				if (drv_data->channel & CHANNEL_LEFT_MASK) {
					if (!(drv_data->channel_state & CHANNEL_LEFT_MASK)) {
						drv_data->channel_state |= CHANNEL_LEFT_MASK;
						utimer_compare_enable_driver(
							drv_data->timer_left_base,
							drv_cfg->channel_left);
					}
					utimer_update_pulse(drv_data->timer_left_base,
							    drv_cfg->channel_left, value);
				}
				if (drv_data->channel & CHANNEL_RIGHT_MASK) {
					if (!(drv_data->channel_state & CHANNEL_RIGHT_MASK)) {
						drv_data->channel_state |= CHANNEL_RIGHT_MASK;
						utimer_compare_enable_driver(
							drv_data->timer_right_base,
							drv_cfg->channel_right);
					}
					utimer_update_pulse(drv_data->timer_right_base,
							    drv_cfg->channel_right, value);
				}
			}
		}
	}

	if (drv_data->mode == PWM_AUDIO_STEREO) {
		if (drv_cfg->resolution > 8) {
			if (ring_buf_size_get(ring) >= 4) {
				ring_buf_get(ring, &wave_l, 1);
				ring_buf_get(ring, &wave_h, 1);
				value_l = ((wave_h << 8) | wave_l);
				ring_buf_get(ring, &wave_l, 1);
				ring_buf_get(ring, &wave_h, 1);
				value_r = ((wave_h << 8) | wave_l);
			} else {
				// fade out
				value_l = value_l_o * FADE_OUT_FACTORY;
				value_r = value_r_old * FADE_OUT_FACTORY;
			}
		} else {
			if (ring_buf_size_get(ring) >= 2) {
				ring_buf_get(ring, &wave_l, 1);
				value_l = wave_l;
				ring_buf_get(ring, &wave_l, 1);
				value_r = wave_l;
			} else {
				// fade out
				value_l = value_l_o * FADE_OUT_FACTORY;
				value_r = value_r_old * FADE_OUT_FACTORY;
			}
		}
		// update audio data to echo channel
		if (value_l != value_l_o) {
			value_l_o = value_l;
			if (value_l == 0) {
				if (drv_data->channel & CHANNEL_LEFT_MASK) {
					if (drv_data->channel_state & CHANNEL_LEFT_MASK) {
						drv_data->channel_state &= ~CHANNEL_LEFT_MASK;
					}
				}
			} else {
				if (drv_data->channel & CHANNEL_LEFT_MASK) {
					if (!(drv_data->channel_state & CHANNEL_LEFT_MASK)) {
						drv_data->channel_state |= CHANNEL_LEFT_MASK;
					}
				}
			}
		}

		if (value_r != value_r_old) {
			value_r_old = value_r;
			if (value_r == 0) {
				if (drv_data->channel & CHANNEL_RIGHT_MASK) {
					if (drv_data->channel_state & CHANNEL_RIGHT_MASK) {
						drv_data->channel_state &= ~CHANNEL_RIGHT_MASK;
					}
				}
			} else {
				if (drv_data->channel & CHANNEL_RIGHT_MASK) {
					if (!(drv_data->channel_state & CHANNEL_RIGHT_MASK)) {
						drv_data->channel_state |= CHANNEL_RIGHT_MASK;
					}
				}
			}
		}
	}

	uint32_t free_size = ring_buf_space_get(ring);
	if (0 == drv_data->ring_buf_give && free_size >= 256) {
		drv_data->ring_buf_give = 1;
		k_sem_give(&drv_data->ring_buf_sem);
	}
}

static inline uint32_t get_period_cycle(uint8_t resolution)
{
	switch (resolution) {
	case 8:
		return 255;
	case 9:
		return 511;
	case 10:
		return 1023;
	case 11:
		return 2047;
	case 12:
		return 4095;
	default:
		return 255;
	}
}

static int alif_pwm_audio_configure(const struct device *dev, struct pwm_audio_stream_cfg *config)
{
	const struct pwm_audio_drv_cfg *drv_cfg = dev->config;
	struct pwm_audio_drv_data *drv_data = dev->data;

	if (config->sample_rate != 8000 && config->sample_rate != 16000) {
		return -ENOTSUP;
	}
	if (config->bits != 8 && config->bits != 16) {
		return -ENOTSUP;
	}
	if (config->mode != PWM_AUDIO_MONO && config->mode != PWM_AUDIO_STEREO) {
		return -ENOTSUP;
	}

	drv_data->sample_rate = config->sample_rate;
	drv_data->bits = config->bits;
	drv_data->mode = config->mode;
	drv_data->is_signed = config->is_signed;

	alif_utimer_enable_timer_clock(drv_cfg->regs, drv_cfg->timer);
	alif_utimer_set_up_counter(drv_data->timer_base);
	uint32_t count = 160000000 / drv_data->sample_rate;
	alif_utimer_set_counter_reload_value(drv_data->timer_base, count);
	alif_utimer_enable_interrupt(drv_data->timer_base, CHAN_INTERRUPT_OVER_FLOW_BIT);
	alif_utimer_enable_counter(drv_data->timer_base);
	alif_utimer_set_counter_value(drv_data->timer_base, 0);
	alif_utimer_enable_soft_counter_ctrl(drv_data->timer_base);

	/* enable pwm */
	if (drv_cfg->timer_left >= 0) {
		alif_utimer_enable_timer_clock(drv_cfg->regs, drv_cfg->timer_left);
		alif_utimer_enable_soft_counter_ctrl(drv_data->timer_left_base);

		alif_utimer_set_up_counter(drv_data->timer_left_base);

		alif_utimer_set_counter_reload_value(drv_data->timer_left_base,
						     get_period_cycle(drv_cfg->resolution));
		alif_utimer_set_counter_value(drv_data->timer_left_base, 0);
		alif_utimer_enable_counter(drv_data->timer_left_base);
	}
	if (drv_cfg->timer_right >= 0 && drv_cfg->timer_right != drv_cfg->timer_left) {
		alif_utimer_enable_timer_clock(drv_cfg->regs, drv_cfg->timer_right);
		alif_utimer_enable_soft_counter_ctrl(drv_data->timer_right_base);

		alif_utimer_set_up_counter(drv_data->timer_right_base);

		alif_utimer_set_counter_reload_value(drv_data->timer_right_base,
						     get_period_cycle(drv_cfg->resolution));
		alif_utimer_set_counter_value(drv_data->timer_right_base, 0);
		alif_utimer_enable_counter(drv_data->timer_right_base);
	}

	/* enable driver output and default driver settings */
	uint32_t reg = (COMPARE_CTRL_DRV_COMPARE_EN |
			(COMPARE_CTRL_DRV_LOW_AT_COMP_MATCH & COMPARE_CTRL_DRV_COMP_MATCH_Msk) |
			(COMPARE_CTRL_DRV_HIGH_AT_CYCLE_END & COMPARE_CTRL_DRV_CYCLE_END_Msk));

	if (drv_cfg->timer_left >= 0) {
		utimer_config_driver(drv_data->timer_left_base, drv_cfg->channel_left, reg);
		utimer_enable_output(drv_cfg->regs, drv_cfg->channel_left, drv_cfg->timer_left);
	}

	if (drv_cfg->timer_right >= 0) {
		utimer_config_driver(drv_data->timer_right_base, drv_cfg->channel_right, reg);
		utimer_enable_output(drv_cfg->regs, drv_cfg->channel_right, drv_cfg->timer_right);
	}

	/* disable all buffer operations */

	drv_data->configured = true;

	return 0;
}

static int alif_pwm_audio_trigger(const struct device *dev, enum pwm_audio_trigger cmd)
{
	const struct pwm_audio_drv_cfg *drv_cfg = dev->config;
	struct pwm_audio_drv_data *drv_data = dev->data;
	if (!drv_data->configured) {
		return -EIO;
	}
	switch (cmd) {
	case PWM_AUDIO_TRIGGER_START:
		ring_buf_reset(&drv_data->ring);
		alif_utimer_set_counter_value(drv_data->timer_base, 0);
		alif_utimer_start_counter(drv_cfg->regs, drv_cfg->timer);
		if (drv_cfg->timer_left >= 0) {
			utimer_update_pulse(drv_data->timer_left_base, drv_cfg->channel_left, 0);
			alif_utimer_set_counter_value(drv_data->timer_left_base, 0);
			alif_utimer_start_counter(drv_cfg->regs, drv_cfg->timer_left);
			// utimer_compare_enable_driver(drv_data->timer_left_base,
			// 			     drv_cfg->channel_left);
		}

		if (drv_cfg->timer_right >= 0 && drv_cfg->timer_right != drv_cfg->timer_left) {
			utimer_update_pulse(drv_data->timer_right_base, drv_cfg->channel_right, 0);
			alif_utimer_set_counter_value(drv_data->timer_right_base, 0);
			alif_utimer_start_counter(drv_cfg->regs, drv_cfg->timer_right);
			// utimer_compare_enable_driver(drv_data->timer_right_base,
			// 			     drv_cfg->channel_right);
		}

		k_sleep(K_MSEC(50));
		if (drv_cfg->power.port != NULL) {
			gpio_pin_set_dt(&drv_cfg->power, 1);
		}
		break;
	case PWM_AUDIO_TRIGGER_STOP:
		ring_buf_reset(&drv_data->ring);
		k_sleep(K_MSEC(50)); // time to fade out
		if (drv_cfg->timer_left >= 0) {
			alif_utimer_stop_counter(drv_cfg->regs, drv_cfg->timer_left);
		}
		if (drv_cfg->timer_right >= 0 && drv_cfg->timer_right != drv_cfg->timer_left) {
			alif_utimer_stop_counter(drv_cfg->regs, drv_cfg->timer_right);
		}
		alif_utimer_stop_counter(drv_cfg->regs, drv_cfg->timer);
		if (drv_cfg->power.port != NULL) {
			gpio_pin_set_dt(&drv_cfg->power, 0);
		}
		break;
	case PWM_AUDIO_TRIGGER_PAUSE:
		alif_utimer_stop_counter(drv_cfg->regs, drv_cfg->timer);
		if (drv_cfg->timer_left >= 0) {
			alif_utimer_stop_counter(drv_cfg->regs, drv_cfg->timer_left);
		}
		if (drv_cfg->timer_right >= 0 && drv_cfg->timer_right != drv_cfg->timer_left) {
			alif_utimer_stop_counter(drv_cfg->regs, drv_cfg->timer_right);
		}
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

static int alif_pwm_audio_write(const struct device *dev, const uint8_t *inbuf, size_t inbuf_len,
				size_t *bytes_written, int32_t timeout)
{
	if (!dev || !inbuf || !bytes_written) {
		return -EINVAL; // Invalid arguments
	}

	int ret = 0;
	const struct pwm_audio_drv_cfg *drv_cfg = dev->config;
	struct pwm_audio_drv_data *drv_data = dev->data;
	struct ring_buf *ring = &drv_data->ring;
	if (!drv_data->configured) {
		return -EIO;
	}

	uint32_t start_ticks = k_uptime_get_32();

	if (ring_buf_is_empty(ring)) {
		drv_data->is_fade_in = true;
		drv_data->fade_in_count = 0;
		drv_data->fade_in_max = FADE_IN_MAX;
	}

	if (drv_data->is_fade_in && (drv_data->fade_in_count >= drv_data->fade_in_max)) {
		drv_data->is_fade_in = false;
	}

	*bytes_written = 0;
	while (inbuf_len) {
		drv_data->ring_buf_give = 0;

		if (k_sem_take(&drv_data->ring_buf_sem, K_MSEC(timeout)) == 0) {
			uint32_t free = ring_buf_space_get(ring);
			uint32_t bytes_can_write = inbuf_len;
			if (inbuf_len > free) {
				bytes_can_write = free;
			}

			bytes_can_write &= 0xfffffffc; /**< Aligned data, bytes_can_write should be
							  an integral multiple of 4 */

			if (0 == bytes_can_write) {
				*bytes_written += inbuf_len; /**< Discard the last misaligned bytes
								of data directly */
				return 0;
			}

			/**< Get the difference between PWM resolution and audio samplewidth */
			int8_t shift = drv_data->bits - drv_cfg->resolution;
			uint32_t len = bytes_can_write;
			switch (drv_data->bits) {
			case 8: {
				if (shift < 0) {
					/**< When the PWM resolution is greater than 8 bits, the
					 * value needs to be expanded */
					uint16_t value;
					uint8_t temp;
					shift = -shift;
					len >>= 1;
					bytes_can_write >>= 1;

					for (size_t i = 0; i < len; i++) {
						temp = inbuf[i];
						if (drv_data->is_signed) {
							temp += 0x7f;
						}
						value = value * drv_data->volume / VOLUME_0DB;
						if (drv_data->is_fade_in &&
						    drv_data->fade_in_count <
							    drv_data->fade_in_max) {
							value = value * drv_data->fade_in_count /
								drv_data->fade_in_max;
							drv_data->fade_in_count++;
						}
						value = temp << shift;
						ring_buf_put(ring, (uint8_t *)&value, 2);
					}
				} else {
					uint8_t value;
					for (size_t i = 0; i < len; i++) {
						value = inbuf[i];
						if (drv_data->is_signed) {
							value += 0x7f;
						}
						value = value * drv_data->volume / VOLUME_0DB;
						if (drv_data->is_fade_in &&
						    drv_data->fade_in_count <
							    drv_data->fade_in_max) {
							value = value * drv_data->fade_in_count /
								drv_data->fade_in_max;
							drv_data->fade_in_count++;
						}
						ring_buf_put(ring, (uint8_t *)&value, 1);
					}
				}
			} break;

			case 16: {
				len >>= 1;
				uint16_t *buf_16b = (uint16_t *)inbuf;
				static uint16_t value_16b;

				if (drv_cfg->resolution > 8) {

					for (size_t i = 0; i < len; i++) {
						value_16b = buf_16b[i];
						if (drv_data->is_signed) {
							value_16b += 0x7fff;
						}
						value_16b =
							value_16b * drv_data->volume / VOLUME_0DB;
						if (drv_data->is_fade_in &&
						    drv_data->fade_in_count <
							    drv_data->fade_in_max) {
							value_16b = value_16b *
								    drv_data->fade_in_count /
								    drv_data->fade_in_max;
							drv_data->fade_in_count++;
						}
						value_16b = value_16b >> shift;
						ring_buf_put(ring, (uint8_t *)&value_16b, 2);
					}
				} else {
					/**
					 * When the PWM resolution is 8 bit, only one byte is
					 * transmitted
					 */
					for (size_t i = 0; i < len; i++) {
						value_16b = buf_16b[i];
						if (drv_data->is_signed) {
							value_16b += 0x7fff;
						}
						value_16b =
							value_16b * drv_data->volume / VOLUME_0DB;
						if (drv_data->is_fade_in &&
						    drv_data->fade_in_count <
							    drv_data->fade_in_max) {
							value_16b = value_16b *
								    drv_data->fade_in_count /
								    drv_data->fade_in_max;
							drv_data->fade_in_count++;
						}
						value_16b = value_16b >> shift;
						ring_buf_put(ring, (uint8_t *)&value_16b, 1);
					}
				}
			} break;

			default:
				break;
			}

			inbuf += bytes_can_write;
			inbuf_len -= bytes_can_write;
			*bytes_written += bytes_can_write;

		} else {
			ret = -ETIMEDOUT;
		}

		if ((k_uptime_get_32() - start_ticks) >= timeout) {
			return -ETIMEDOUT;
		}
	}

	return ret; // Success
}

static int alif_pwm_audio_set_volume(const struct device *dev, uint8_t volume)
{
	// const struct pwm_audio_drv_cfg *drv_cfg = dev->config;
	struct pwm_audio_drv_data *drv_data = dev->data;
	if (!drv_data->configured) {
		return -EIO;
	}
	// 0 - 100 50 - 0db
	if (volume > 100) {
		volume = 100;
	}

	drv_data->volume = VOLUME_0DB + (volume - VOLUME_0DB);

	return 0;
}
static int alif_pwm_audio_init(const struct device *dev)
{
	const struct pwm_audio_drv_cfg *drv_cfg = dev->config;
	struct pwm_audio_drv_data *drv_data = dev->data;

	if (drv_cfg->pcfg) {
		int ret = pinctrl_apply_state(drv_cfg->pcfg, PINCTRL_STATE_DEFAULT);
		if (ret) {
			LOG_ERR("Failed to apply pin configuration: %d", ret);
			return ret;
		}
	}

	if (drv_cfg->power.port) {
		if (!device_is_ready(drv_cfg->power.port)) {
			LOG_ERR("Failed to get power device");
			return -ENODEV;
		}
		int ret = gpio_pin_configure_dt(&drv_cfg->power, GPIO_OUTPUT_INACTIVE);
		if (ret) {
			LOG_ERR("Failed to configure power pin: %d", ret);
			return ret;
		}
	}

	if (drv_cfg->irq_config) {
		drv_cfg->irq_config(dev);
	}

	drv_data->timer_base = drv_cfg->regs + (drv_cfg->timer + 1) * 0x1000;

	if (drv_cfg->channel_left != -1) {
		drv_data->timer_left_base = drv_cfg->regs + (drv_cfg->timer_left + 1) * 0x1000;
		drv_data->channel |= CHANNEL_LEFT_MASK;
	}
	if (drv_cfg->channel_right != -1) {
		drv_data->timer_right_base = drv_cfg->regs + (drv_cfg->timer_right + 1) * 0x1000;
		drv_data->channel |= CHANNEL_RIGHT_MASK;
	}
	drv_data->sample_rate = 8000;
	drv_data->bits = 8;
	drv_data->volume = VOLUME_0DB;
	drv_data->ring_buf_give = 0;

	ring_buf_init(&drv_data->ring, drv_cfg->ring_buf_size, drv_cfg->ring_buf);
	k_sem_init(&drv_data->ring_buf_sem, 1, 1);

	return 0;
}

static const struct _pwm_audio_ops alif_pwm_audio_api __maybe_unused = {
	.configure = alif_pwm_audio_configure,
	.trigger = alif_pwm_audio_trigger,
	.write = alif_pwm_audio_write,
	.set_vol = alif_pwm_audio_set_volume,
};

/* Generic audio interface adaptation functions */
static int pwm_audio_generic_configure(const struct device *dev, const struct audio_config *config)
{
	struct pwm_audio_stream_cfg pwm_config = {
		.sample_rate = config->sample_rate,
		.bits = config->bits_per_sample,
		.mode = (config->channels == AUDIO_STEREO) ? PWM_AUDIO_STEREO : PWM_AUDIO_MONO,
		.is_signed = config->is_signed,
	};

	return alif_pwm_audio_configure(dev, &pwm_config);
}

static int pwm_audio_generic_set_volume(const struct device *dev, uint8_t volume)
{
	return alif_pwm_audio_set_volume(dev, volume);
}

static int pwm_audio_generic_trigger(const struct device *dev, enum audio_trigger cmd)
{
	enum pwm_audio_trigger pwm_cmd;

	switch (cmd) {
	case AUDIO_TRIGGER_START:
		pwm_cmd = PWM_AUDIO_TRIGGER_START;
		break;
	case AUDIO_TRIGGER_STOP:
		pwm_cmd = PWM_AUDIO_TRIGGER_STOP;
		break;
	case AUDIO_TRIGGER_PAUSE:
		pwm_cmd = PWM_AUDIO_TRIGGER_PAUSE;
		break;
	case AUDIO_TRIGGER_RESUME:
		pwm_cmd = PWM_AUDIO_TRIGGER_START; /* Resume is same as start for PWM */
		break;
	case AUDIO_TRIGGER_DRAIN:
		/* PWM audio doesn't have explicit drain, just return success */
		return 0;
	default:
		return -EINVAL;
	}

	return alif_pwm_audio_trigger(dev, pwm_cmd);
}

static int pwm_audio_generic_write(const struct device *dev, const void *data, size_t size,
				   size_t *bytes_written, k_timeout_t timeout)
{
	return alif_pwm_audio_write(dev, (const uint8_t *)data, size, bytes_written, timeout.ticks);
}

static int pwm_audio_generic_read(const struct device *dev, void *data, size_t size,
				  size_t *bytes_read, k_timeout_t timeout)
{
	/* PWM audio is output-only */
	return -ENOSYS;
}

static const struct audio_driver_api pwm_audio_generic_api = {
	.configure = pwm_audio_generic_configure,
	.set_volume = pwm_audio_generic_set_volume,
	.trigger = pwm_audio_generic_trigger,
	.write = pwm_audio_generic_write,
	.read = pwm_audio_generic_read,
};

#define ALIF_PWM_AUDIO_INIT(inst)                                                                  \
	IF_ENABLED(CONFIG_PINCTRL, (PINCTRL_DT_INST_DEFINE(inst);))                                \
	static uint8_t alif_pwm_audio_buf_##inst[DT_INST_PROP(inst, ringbuf_size)];                \
	static void alif_pdm_irq_config_func_##inst(const struct device *dev)                      \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(inst), DT_INST_IRQ(inst, priority), timer_isr,            \
			    DEVICE_DT_INST_GET(inst), 0);                                          \
		irq_enable(DT_INST_IRQN(inst));                                                    \
	};                                                                                         \
	static struct pwm_audio_drv_cfg alif_pwm_audio_config_##inst = {                           \
		.regs = (uint32_t)DT_INST_REG_ADDR(inst),                                          \
		.resolution = (uint8_t)DT_INST_PROP(inst, resolution),                             \
		.power = GPIO_DT_SPEC_INST_GET_OR(inst, power_gpios, {0}),                         \
		.timer = (uint8_t)DT_INST_PROP(inst, timer),                                       \
		.timer_left = (uint8_t)DT_INST_PROP_BY_IDX(inst, left, 0),                         \
		.timer_right = (uint8_t)DT_INST_PROP_BY_IDX(inst, right, 0),                       \
		.channel_left = (uint8_t)DT_INST_PROP_BY_IDX(inst, left, 1),                       \
		.channel_right = (uint8_t)DT_INST_PROP_BY_IDX(inst, right, 1),                     \
		.irq_config = alif_pdm_irq_config_func_##inst,                                     \
		.ring_buf = alif_pwm_audio_buf_##inst,                                             \
		.ring_buf_size = DT_INST_PROP(inst, ringbuf_size),                                 \
		IF_ENABLED(CONFIG_PINCTRL, (.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst), ))};     \
	static struct pwm_audio_drv_data pwm_audio_drv_data##inst = {                              \
		.timer_base = 1,                                                                   \
		.timer_left_base = 0,                                                              \
		.timer_right_base = 0,                                                             \
		.sample_rate = 0,                                                                  \
		.bits = 0,                                                                         \
		.mode = PWM_AUDIO_MONO,                                                            \
		.channel = 0,                                                                      \
		.ring = {0},                                                                       \
		.configured = false,                                                               \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, &alif_pwm_audio_init, NULL, &pwm_audio_drv_data##inst,         \
			      &alif_pwm_audio_config_##inst, POST_KERNEL,                          \
			      CONFIG_PWM_AUDIO_INIT_PRIORITY,                                      \
			      COND_CODE_1(CONFIG_PWM_AUDIO_GENERIC_API, (&pwm_audio_generic_api),  \
					  (&alif_pwm_audio_api)));

DT_INST_FOREACH_STATUS_OKAY(ALIF_PWM_AUDIO_INIT);