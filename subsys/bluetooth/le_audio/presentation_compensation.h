/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef _PRESENTATION_COMPENSATION_H
#define _PRESENTATION_COMPENSATION_H

#include <zephyr/types.h>
#include <zephyr/device.h>

/**
 * @brief Presentation compensation callback signature
 *
 * This callback is used to notify listeners of the amount of correction needed to adjust
 * presentation delay of the next audio frame. This is measured in microseconds. A positive number
 * indicates that audio samples (generally zeros) should be added to make up the given time, and a
 * negative number indicates that audio samples should be dropped.
 *
 * @param correction The correction required in microseconds
 */
typedef void (*presentation_compensation_cb_t)(int32_t correction_us);

/**
 * @brief Configure the presentation compensation module
 *
 * @param clock_dev The clock device used to adjust audio playback speed. This device must support
 * the clock_control.h API.
 * @param presentation_delay_us The target presentation delay in microseconds
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int presentation_compensation_configure(const struct device *clock_dev,
					uint32_t presentation_delay_us);

/**
 * @brief Notify the presentation compensation module of the actual presentation delay of a newly
 * sent SDU
 *
 * @param presentation_delay_us The actual presentation delay of the SDU in microseconds
 */
void presentation_compensation_notify_timing(uint32_t presentation_delay_us);

/**
 * @brief Register a callback to be notified of what correction (if any) should be applied to the
 * next audio frame.
 *
 * @param cb Callback to register
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int presentation_compensation_register_cb(presentation_compensation_cb_t cb);

#ifdef CONFIG_PRESENTATION_COMPENSATION_DEBUG
struct presentation_comp_debug_data {
	int32_t err_us;
	int32_t correction_us;
	uint32_t clock_freq;
	float pi_output;
	float pi_integrator;
} __attribute__((__packed__));

typedef void (*presentation_comp_debug_cb_t)(struct presentation_comp_debug_data *dbg_data);

/**
 * @brief Register a callback to be executed when buffer of debug data is filled.
 *
 * @param cb Callback function to register
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int presentation_compensation_register_debug_cb(presentation_comp_debug_cb_t cb);
#endif /* CONFIG_PRESENTATION_COMPENSATION_DEBUG */

#endif /* _PRESENTATION_COMPENSATION_H */
