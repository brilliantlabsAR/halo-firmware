/**
 * @file main.c
 * @brief Ship Mode Sample
 *
 * Copyright (C) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sm/sm.h>

#define COUNTDOWN_SECONDS  5

int main(void)
{
	const struct device *sm = DEVICE_DT_GET(DT_ALIAS(shutdown));

	printk("\n=== Ship Mode Sample ===\n\n");

	if (!device_is_ready(sm)) {
		printk("ERROR: Ship mode device not ready!\n");
		return -1;
	}

	printk("WARNING: Device will shutdown in %d seconds\n", COUNTDOWN_SECONDS);
	printk("Hardware reset required to wake up!\n\n");

	for (int i = COUNTDOWN_SECONDS; i > 0; i--) {
		printk("Shutdown in %d...\n", i);
		k_sleep(K_SECONDS(1));
	}

	printk("\nShutting down now!\n");
	
	shutdown(sm);

	/* Should not reach here if ship mode works correctly */
	k_sleep(K_SECONDS(1));
	printk("WARNING: Still running! Check hardware.\n");

	while (1) {
		k_sleep(K_FOREVER);
	}
	
	return 0;
}
