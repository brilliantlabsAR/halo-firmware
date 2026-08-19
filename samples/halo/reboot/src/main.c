/*
 * Copyright (c) 2015 Intel Corporation
 * Copyright (c) 2018 Nordic Semiconductor
 * Copyright (c) 2019 Centaur Analytics, Inc
 * Copyright 2023 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <stdbool.h>
#include "alif_ble.h"

int main(void)
{

	printk("Reboot sample application\n");

	printk("BLE initialized\n");
	alif_ble_enable(NULL);

	for (int i = 5; i > 0; i--) {
		printk("Waiting for reset...\n");
		k_sleep(K_MSEC(1000));
	}

	printk("Rebooting...\n");
	sys_reboot(SYS_REBOOT_COLD);

	while (1) {
		k_yield();
	}

	return 0;
}
