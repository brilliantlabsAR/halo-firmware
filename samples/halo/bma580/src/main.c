/*
 * Copyright (c)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <stdio.h>
#include <zephyr/sys/util.h>

static int print_samples = 0;

static struct sensor_value accel_x_out, accel_y_out, accel_z_out, temp_out;

int main(void)
{
	char out_str[512];
	struct sensor_value odr_attr;
	const struct device *const bma580_dev = DEVICE_DT_GET_ONE(bosch_bma580);

	if (!device_is_ready(bma580_dev)) {
		printk("sensor: device not ready.\n");
		return 0;
	}

	/* set accelgyro sampling frequency to 200 Hz */
	odr_attr.val1 = 200;
	odr_attr.val2 = 0;

	if (sensor_attr_set(bma580_dev, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY,
			    &odr_attr) < 0) {
		printk("Cannot set sampling frequency for accelerometer.\n");
		return 0;
	}

	while (1) {
		/* Erase previous */
		printk("\0033\014");
		printf("bma580 sensor samples: %d\n\n", print_samples++);

		if (sensor_sample_fetch(bma580_dev) < 0) {
			printk("Sensor sample update error\n");
			return 0;
		}

		sensor_channel_get(bma580_dev, SENSOR_CHAN_ACCEL_X, &accel_x_out);
		sensor_channel_get(bma580_dev, SENSOR_CHAN_ACCEL_Y, &accel_y_out);
		sensor_channel_get(bma580_dev, SENSOR_CHAN_ACCEL_Z, &accel_z_out);
		sensor_channel_get(bma580_dev, SENSOR_CHAN_DIE_TEMP, &temp_out);

		/* bma580 accel */
		sprintf(out_str, "accel x:%f ms/2 y:%f ms/2 z:%f ms/2 temp:%f C\n",
			sensor_value_to_double(&accel_x_out), sensor_value_to_double(&accel_y_out),
			sensor_value_to_double(&accel_z_out), sensor_value_to_double(&temp_out));

		printk("%s\n", out_str);

		k_sleep(K_MSEC(2000));
	}
}
