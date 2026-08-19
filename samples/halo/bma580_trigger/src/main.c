/*
 * Copyright (c)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/i2c.h>
#include <stdio.h>
#include <zephyr/sys/util.h>

struct bma580_config {
	struct i2c_dt_spec i2c;
};

static int print_samples = 0;

static struct sensor_value accel_x_out, accel_y_out, accel_z_out, temp_out;
static void bma580_trigger_handler(const struct device *dev, const struct sensor_trigger *trig)
{
	switch (trig->type) {
	case SENSOR_TRIG_DATA_READY:
		printk("Data ready triggered at %d\n", k_uptime_get_32());
		break;
	case SENSOR_TRIG_TAP:
		printk("Single tap triggered at %d\n", k_uptime_get_32());
		break;
	case SENSOR_TRIG_DOUBLE_TAP:
		printk("Double tap triggered at %d\n", k_uptime_get_32());
		break;
	default:
		printk("Unknown trigger type %d at %d\n", trig->type, k_uptime_get_32());
		break;
	}
}

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

	struct sensor_trigger strig;

	
	strig.type = SENSOR_TRIG_TAP;
	strig.chan = SENSOR_CHAN_ACCEL_Z;

	if (sensor_trigger_set(bma580_dev, &strig, bma580_trigger_handler) != 0) {
		printk("Could not set sensor type and channel\n");
		return 0;
	} else {
		printk("Tap trigger set successfully\n");
	}

	struct sensor_trigger dtrig;

	dtrig.type = SENSOR_TRIG_DOUBLE_TAP;
	dtrig.chan = SENSOR_CHAN_ACCEL_Z;
	if (sensor_trigger_set(bma580_dev, &dtrig, bma580_trigger_handler) != 0) {
		printk("Could not set sensor type and channel\n");
		return 0;
	} else {
		printk("Double tap trigger set successfully\n");
	}


	

	while (1) {
		/* Erase previous */
		printk("bma580 sensor samples: %d\n\n", print_samples++);

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
		printk(".");

		k_sleep(K_MSEC(2000));

		/* Check interrupt status manually */
		uint8_t int_stat_0, int_stat_1;
		const struct bma580_config *cfg = (const struct bma580_config *)bma580_dev->config;
		i2c_reg_read_byte_dt(&cfg->i2c, 0x12, &int_stat_0);
		i2c_reg_read_byte_dt(&cfg->i2c, 0x13, &int_stat_1);
		if (int_stat_0 & 0x80) {
			printk("Manual check: STAP interrupt status set\n");
		}
		if (int_stat_1 & 0x01) {
			printk("Manual check: DTAP interrupt status set\n");
		}
	}
}
