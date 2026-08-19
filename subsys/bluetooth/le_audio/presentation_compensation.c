/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/clock_control.h>
#include <string.h>
#include "presentation_compensation.h"

LOG_MODULE_REGISTER(presentation_compensation, CONFIG_BLE_AUDIO_LOG_LEVEL);

#define MICROSECONDS_PER_SECOND 1000000

#if CONFIG_ALIF_BLE_AUDIO_FRAME_DURATION_10MS
#define SECONDS_PER_FRAME       0.01f
#else
#error "Unsupported configuration"
#endif

BUILD_ASSERT(CONFIG_PRESENTATION_COMPENSATION_CORRECTION_FACTOR != 0,
	     "Correction factor cannot be zero");

struct presentation_compensation_stats {
	int32_t err_max;
	int32_t err_min;
	int32_t err_last;
	int32_t err_sum;
	int32_t err_count;
	uint32_t total_us_dropped;
	uint32_t total_us_silence;
};

struct presentation_compensation_env {
	const struct device *clock_dev;
	uint32_t target_delay_us;
	presentation_compensation_cb_t cb;
	float integrator;
	uint32_t initial_freq;
	uint32_t last_freq;
#ifdef CONFIG_PRESENTATION_COMPENSATION_PRINT_STATS
	struct presentation_compensation_stats stats;
#endif
};

static struct presentation_compensation_env env;

#ifdef CONFIG_PRESENTATION_COMPENSATION_DEBUG
static presentation_comp_debug_cb_t dbg_cb;
static struct presentation_comp_debug_data
	debug_data[CONFIG_PRESENTATION_COMPENSATION_DEBUG_SAMPLES];
static uint32_t debug_index;
#endif

#ifdef CONFIG_PRESENTATION_COMPENSATION_PRINT_STATS
static void reset_stats(void)
{
	env.stats.err_last = 0;
	env.stats.err_min = INT32_MAX;
	env.stats.err_max = INT32_MIN;
	env.stats.err_sum = 0;
	env.stats.err_count = 0;
}
#endif

int presentation_compensation_configure(const struct device *clock_dev,
					uint32_t presentation_delay_us)
{
	if (!device_is_ready(clock_dev)) {
		LOG_ERR("Clock device is not ready");
		return -ENODEV;
	}

	env.clock_dev = clock_dev;
	env.target_delay_us = presentation_delay_us;
	env.integrator = 0.0f;

	uint32_t clock_rate;
	int ret = clock_control_get_rate(clock_dev, NULL, &clock_rate);

	if (ret) {
		LOG_ERR("Failed to get rate from clock device");
		return ret;
	}

	env.initial_freq = clock_rate / CONFIG_AUDIO_CLOCK_DIVIDER;
	env.last_freq = env.initial_freq;

#ifdef CONFIG_PRESENTATION_COMPENSATION_PRINT_STATS
	reset_stats();
	env.stats.total_us_dropped = 0;
	env.stats.total_us_silence = 0;
#endif

	return 0;
}

static void adjust_clock(int32_t delta_f)
{
	uint32_t freq = env.initial_freq + delta_f;

	if (freq == env.last_freq) {
		/* No adjustment required */
		return;
	}

	/* Check against absolute frequency limits */
	uint32_t diff_from_centre =
		(freq > env.initial_freq) ? (freq - env.initial_freq) : (env.initial_freq - freq);

	if (diff_from_centre > CONFIG_PRESENTATION_COMPENSATION_MAX_DELTA_F) {
		if (freq > env.initial_freq) {
			freq = env.initial_freq + CONFIG_PRESENTATION_COMPENSATION_MAX_DELTA_F;
		} else {
			freq = env.initial_freq - CONFIG_PRESENTATION_COMPENSATION_MAX_DELTA_F;
		}
	}

	/* Check against incremental frequency limits */
	uint32_t diff_from_last =
		(freq > env.last_freq) ? (freq - env.last_freq) : (env.last_freq - freq);

	if (diff_from_last > CONFIG_PRESENTATION_COMPENSATION_MAX_INCREMENTAL_DELTA_F) {
		if (freq > env.last_freq) {
			freq = env.last_freq +
			       CONFIG_PRESENTATION_COMPENSATION_MAX_INCREMENTAL_DELTA_F;
		} else {
			freq = env.last_freq -
			       CONFIG_PRESENTATION_COMPENSATION_MAX_INCREMENTAL_DELTA_F;
		}
	}

	/* Perform clock adjustment */
	uint64_t new_freq = freq * CONFIG_AUDIO_CLOCK_DIVIDER;
	int ret = clock_control_set_rate(env.clock_dev, NULL, &new_freq);

	if (ret) {
		LOG_ERR("Failed to adjust clock frequency, err %d", ret);
		return;
	}

	env.last_freq = freq;
}

static float run_clock_pi_controller(int32_t err)
{
	float output;
	float output_saturated;

	/* Only integer values can be specified in Kconfig. Since the actual number has no physical
	 * meaning, divide by 10 and convert to floating point here to make some more resolution
	 * available
	 */
	const float Kp = CONFIG_PRESENTATION_COMPENSATION_KP / 10.0f;
	const float Ki = CONFIG_PRESENTATION_COMPENSATION_KI / 10.0f;

#if defined(CONFIG_PRESENTATION_COMPENSATION_DIRECTION_SOURCE)
	/* Error value is already correct, no inversion needed */
#elif defined(CONFIG_PRESENTATION_COMPENSATION_DIRECTION_SINK)
	/* Error value must be inverted, a positive error means clock should go slower */
	err = -1 * err;
#else
#error "Either sink or source direction must be defined"
#endif

	output = Kp * err + Ki * env.integrator;

	/* Saturate the output */
	if (output > CONFIG_PRESENTATION_COMPENSATION_MAX_DELTA_F) {
		output_saturated = CONFIG_PRESENTATION_COMPENSATION_MAX_DELTA_F;
	} else if (output < -CONFIG_PRESENTATION_COMPENSATION_MAX_DELTA_F) {
		output_saturated = -CONFIG_PRESENTATION_COMPENSATION_MAX_DELTA_F;
	} else {
		output_saturated = output;
	}

	/* Conditional integration */
	if ((output > output_saturated) && (err > 0)) {
		/* Output is saturated to max, and integral term would increase output --> don't
		 * integrate error
		 */
	} else if ((output < output_saturated) && (err < 0)) {
		/* Output is saturated to min, and integral term would decrease output --> don't
		 * integrate error
		 */
	} else {
		env.integrator += err * SECONDS_PER_FRAME;
	}

	adjust_clock(output_saturated);
	return output_saturated;
}

static int32_t calculate_correction(int32_t presentation_error_us)
{
	int32_t correction_us = 0;
	float pi_output = 0.0F;

	if ((presentation_error_us + CONFIG_PRESENTATION_COMPENSATION_THRESHOLD_US < 0) ||
	    (presentation_error_us > CONFIG_PRESENTATION_COMPENSATION_THRESHOLD_US)) {
		/* Samples must be dropped or inserted to correct the error */

		correction_us =
			presentation_error_us / CONFIG_PRESENTATION_COMPENSATION_CORRECTION_FACTOR;
	} else {
		/* No samples need to be dropped or inserted, but the clock may need adjustment */
		pi_output = run_clock_pi_controller(presentation_error_us);
	}

#ifdef CONFIG_PRESENTATION_COMPENSATION_DEBUG
	if (debug_index < CONFIG_PRESENTATION_COMPENSATION_DEBUG_SAMPLES) {
		struct presentation_comp_debug_data *dbg_pt = &debug_data[debug_index];

		dbg_pt->err_us = presentation_error_us;
		dbg_pt->correction_us = correction_us;
		dbg_pt->clock_freq = env.last_freq;
		dbg_pt->pi_output = pi_output;
		dbg_pt->pi_integrator = env.integrator;

		debug_index++;

		if ((debug_index == CONFIG_PRESENTATION_COMPENSATION_DEBUG_SAMPLES) && dbg_cb) {
			dbg_cb(debug_data);
		}
	}
#endif

	return correction_us;
}

#ifdef CONFIG_PRESENTATION_COMPENSATION_PRINT_STATS
void update_stats(int32_t presentation_error_us, int32_t correction)
{
	env.stats.err_last = presentation_error_us;

	if (presentation_error_us > env.stats.err_max) {
		env.stats.err_max = presentation_error_us;
	}

	if (presentation_error_us < env.stats.err_min) {
		env.stats.err_min = presentation_error_us;
	}

	env.stats.err_sum += presentation_error_us;
	env.stats.err_count++;

	if (correction < 0) {
		env.stats.total_us_dropped -= correction;
	} else {
		env.stats.total_us_silence += correction;
	}

	if (env.stats.err_count == CONFIG_PRESENTATION_COMPENSATION_PRINT_STATS_INTERVAL) {
		LOG_INF("error last: %d us, min %d us, max %d us | last clock rate %u | "
			"total dropped %u us, total silence %u us",
			env.stats.err_last, env.stats.err_min, env.stats.err_max, env.last_freq,
			env.stats.total_us_dropped, env.stats.total_us_silence);
		reset_stats();
	}
}
#endif

void presentation_compensation_notify_timing(uint32_t presentation_delay_us)
{
	/* Presentation error is defined as the difference between the target presentation delay and
	 * the actual presentation delay. A positive value indicates that the actual presentation
	 * delay was shorter than the target.
	 */
	int32_t presentation_error_us =
		(int32_t)env.target_delay_us - (int32_t)presentation_delay_us;

	/* Calculate required correction and notify listener */
	int32_t correction = calculate_correction(presentation_error_us);

	if (env.cb) {
		env.cb(correction);
	}

#ifdef CONFIG_PRESENTATION_COMPENSATION_PRINT_STATS
	update_stats(presentation_error_us, correction);
#endif
}

int presentation_compensation_register_cb(presentation_compensation_cb_t cb)
{
	if (cb == NULL) {
		return -EINVAL;
	}

	env.cb = cb;

	return 0;
}

#ifdef CONFIG_PRESENTATION_COMPENSATION_DEBUG
int presentation_compensation_register_debug_cb(presentation_comp_debug_cb_t cb)
{
	if (cb == NULL) {
		return -EINVAL;
	}

	dbg_cb = cb;

	return 0;
}
#endif
