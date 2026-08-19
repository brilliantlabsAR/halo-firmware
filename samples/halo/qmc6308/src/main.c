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

static struct sensor_value magn_x_out, magn_y_out, magn_z_out;

int main(void)
{
	char out_str[64];
	struct sensor_value odr_attr;
	const struct device *const qmc6308_dev = DEVICE_DT_GET_ONE(qst_qmc6308);

	if (!device_is_ready(qmc6308_dev)) {
		printk("sensor: device not ready.\n");
		return 0;
	}

	/* set magngyro sampling frequency to 200 Hz */
	odr_attr.val1 = 200;
	odr_attr.val2 = 0;

	if (sensor_attr_set(qmc6308_dev, SENSOR_CHAN_MAGN_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY,
			    &odr_attr) < 0) {
		printk("Cannot set sampling frequency for magnetic.\n");
		return 0;
	}

	while (1) {
		/* Erase previous */
		printk("\0033\014");
		printf("QMC6308 sensor samples: %d\n\n", print_samples++);

		if (sensor_sample_fetch(qmc6308_dev) < 0) {
			printk("Sensor sample update error\n");
			return 0;
		}

		sensor_channel_get(qmc6308_dev, SENSOR_CHAN_MAGN_X, &magn_x_out);
		sensor_channel_get(qmc6308_dev, SENSOR_CHAN_MAGN_Y, &magn_y_out);
		sensor_channel_get(qmc6308_dev, SENSOR_CHAN_MAGN_Z, &magn_z_out);

		/* qmc6308 magn */
		sprintf(out_str, "magn x:%f ms/2 y:%f ms/2 z:%f ms/2",
			sensor_value_to_double(&magn_x_out), sensor_value_to_double(&magn_y_out),
			sensor_value_to_double(&magn_z_out));

		printk("%s\n", out_str);

		k_sleep(K_MSEC(2000));
	}
}
