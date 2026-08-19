/*
 * Copyright (C) 2024 Alif Semiconductor.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/crc/alif_crc.h>
#include <zephyr/sys/printk.h>
#include <stdint.h>

struct crc_params params;

#define TRUE  1
#define FALSE 0

uint8_t arr[] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
uint32_t polynomial = 0x2CEEA6C8;
uint32_t seed_value;

int main(void)
{
	const struct device *const crc_dev = DEVICE_DT_GET(DT_NODELABEL(crc));

	uint32_t crc_output;

	params.data_in     = (uint8_t *)arr;
	params.len         = ARRAY_SIZE(arr);
	params.bit_swap    = FALSE;
	params.byte_swap   = FALSE;
	params.reflect     = FALSE;
	params.invert      = FALSE;
	params.custum_poly = FALSE;
	params.data_out    = &crc_output;

	if (!crc_dev) {
		printk("crc_dev not found\n");
		return -1;
	}

	if (!device_is_ready(crc_dev)) {
		printk("device not ready\n");
		return -1;
	}

	/* Add seed value */
	crc_seed(crc_dev, seed_value);

	/* calculate crc output */
	crc_compute(crc_dev, &params);

	printk("CRC output: 0x%X\n", *params.data_out);

	return 0;
}
