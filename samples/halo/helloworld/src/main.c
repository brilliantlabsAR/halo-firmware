/*
 * Copyright (C) 2024 Alif Semiconductor.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

int main(void)
{
	while (1) {
		printk("Hello World! %s\n", CONFIG_BOARD);
		k_sleep(K_MSEC(1000));
	}

	return 0;
}
