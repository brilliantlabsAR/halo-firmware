/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * BMA580-specific extensions to the Zephyr sensor API: the triple-tap
 * trigger and the tap-detector tuning attributes. All attribute values
 * are raw chip units (see bma580_features.h for field widths).
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_BMA580_SENSOR_H_
#define ZEPHYR_INCLUDE_DRIVERS_BMA580_SENSOR_H_

#include <zephyr/drivers/sensor.h>

enum bma580_sensor_trigger {
	/* Fires on a hardware-detected triple tap (ttap_int). Single and
	 * double tap use the standard SENSOR_TRIG_TAP / SENSOR_TRIG_DOUBLE_TAP. */
	BMA580_TRIG_TRIPLE_TAP = SENSOR_TRIG_PRIV_START,
};

enum bma580_sensor_attribute {
	/* Detection mode: 0 = sensitive, 1 = normal, 2 = robust. */
	BMA580_ATTR_TAP_MODE = SENSOR_ATTR_PRIV_START,
	/* Dominant sensing axis: 0 = X, 1 = Y, 2 = Z. The tap engine watches
	 * exactly one axis (2-bit field; 3 is reserved). */
	BMA580_ATTR_TAP_AXIS,
	/* Minimum peak threshold, 0..1023. */
	BMA580_ATTR_TAP_PEAK_THRES,
	/* Maximum threshold crossings around one tap, 0..7. */
	BMA580_ATTR_TAP_MAX_PEAKS,
	/* Window from the first tap in which the 2nd/3rd must land, 0..63. */
	BMA580_ATTR_TAP_MAX_GESTURE_DUR,
	/* 1 = confirm gesture only after max_gesture_dur expires. */
	BMA580_ATTR_TAP_WAIT_FOR_TIMEOUT,
	/* Max duration between positive and negative peaks of a tap, 0..15. */
	BMA580_ATTR_TAP_MAX_DUR_BETWEEN_PEAKS,
	/* Duration the tap impact is observed (shock settling), 0..15. */
	BMA580_ATTR_TAP_SHOCK_SETTLING_DUR,
	/* Minimum quiet duration between two consecutive taps, 0..15. */
	BMA580_ATTR_TAP_MIN_QUIET_DUR,
	/* Minimum quiet time between two gestures, 0..15. */
	BMA580_ATTR_TAP_QUIET_TIME_AFTER_GESTURE,
};

#endif /* ZEPHYR_INCLUDE_DRIVERS_BMA580_SENSOR_H_ */
