/*
 * Copyright (C) 2024 Alif Semiconductor
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/device.h>
#include <zephyr/drivers/dac/alif_dac.h>
#include <stdint.h>
#include <zephyr/devicetree.h>

#define DAC_NODE	DT_ALIAS(dac)

/* DAC maximum resolution is 12-bit */
#define DAC_MAX_INPUT_VALUE   (0xFFF)
struct dac_channel_cfg dac_cfg;

int main(void)
{
	printk("\r\n >>>Starting up the Zephyr DAC demo!!! <<< \r\n");

	const struct device *const dac_dev = DEVICE_DT_GET(DAC_NODE);
	uint32_t input_value = 0;
	int32_t ret = 0;

	dac_cfg.channel_id = 0;
	dac_cfg.resolution = 12;
	dac_cfg.buffered = 0;

	/* Set DAC capacitance  */
	dac_set_capacitance(dac_dev, DAC_8PF_CAPACITANCE);

	/* Set DAC IBAIS output current */
	dac_set_output_current(dac_dev, DAC_1100UA_OUT_CUR);

	dac_channel_setup(dac_dev, &dac_cfg);

		if (ret != 0) {
			printk("\r\n Setup error:DAC_channel\n");
			return -1;
		}

	input_value = 0;

	while (1) {

		/* set dac input */
		dac_write_value(dac_dev, 0, input_value);

		if (ret != 0) {
			printk("\r\n Error: DAC Set Input failed\n");
			return -1;
		}

		/* Sleep for n micro second */
		k_msleep(2000);

		/* If the input value is equal to maximum
		 * dac input value then input
		 * value will be set to 0
		 */
		if (input_value == DAC_MAX_INPUT_VALUE) {
			input_value = 0;

		/* If the input value is not greater than the maximum
		 * dac input value then input
		 *value will be incremented by 1000 value
		 */
		} else {

			input_value += 1000;
		}
		/* If the input value is maximum than the maximum
		 * DAC input value then the input
		 *value will be set to DAC maximum input value
		 */
		if (input_value > DAC_MAX_INPUT_VALUE) {

			input_value = DAC_MAX_INPUT_VALUE;
		}

	}

	return 0;
}
