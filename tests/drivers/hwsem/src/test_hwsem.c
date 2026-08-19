/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/kernel.h>
#include <string.h>
#include <zephyr/drivers/hwsem_ipm.h>
#include <zephyr/ztest.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#define SLEEP_TIME_MS		 1000
#define SHARED_TEST_ITERATIONS	  7
#define LED0_NODE		DT_ALIAS(led0)

#define DEVICE_DT_GET_AND_COMMA(node_id) DEVICE_DT_GET(node_id),

/* Generate a list of devices for all instances of the "compat" */
#define DEVS_FOR_DT_COMPAT(compat) \
	DT_FOREACH_STATUS_OKAY(compat, DEVICE_DT_GET_AND_COMMA)

static const struct device *const devices[] = {
#ifdef CONFIG_ALIF_HWSEM
	DEVS_FOR_DT_COMPAT(alif_hwsem)
#endif
};

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

#if defined(CONFIG_SOC_E7_AK_APSS) || defined(CONFIG_SOC_E7_DK_APSS)
#define MASTER_ID 0xCAFECAFE
#elif defined(CONFIG_SOC_E3_AK_RTSS_HP) || defined(CONFIG_SOC_E7_AK_RTSS_HP) || \
	defined(CONFIG_SOC_E3_DK_RTSS_HP) || defined(CONFIG_SOC_E7_DK_RTSS_HP)
#define MASTER_ID 0xF00DF00D
#elif defined(CONFIG_SOC_E3_AK_RTSS_HE) || defined(CONFIG_SOC_E7_AK_RTSS_HE) || \
	defined(CONFIG_SOC_E3_DK_RTSS_HE) || defined(CONFIG_SOC_E7_DK_RTSS_HE)
#define MASTER_ID 0xC0DEC0DE
#endif

/* Test Case to validate Initialization of all HWSEM Nodes */
ZTEST(hwsem_basic, test_initialize)
{
	int device_idx;

	printk("Test all %d Hardware Semaphores(HWSEM) on %s\n",
	       (int)ARRAY_SIZE(devices), CONFIG_BOARD);

	for (device_idx = 0; device_idx < ARRAY_SIZE(devices); device_idx++) {
		zassert_true(device_is_ready(devices[device_idx]),
			     "HWSEM device %d not ready", device_idx);
	}
}

/* Test Case to lock all HWSEM Nodes using lock API */
ZTEST(hwsem_basic, test_lock)
{
	int device_idx;

	for (device_idx = 0; device_idx < ARRAY_SIZE(devices); device_idx++) {
		zassert_false(hwsem_lock(devices[device_idx], MASTER_ID),
			      "Unable to lock HWSEM %d\n", device_idx);
		/* Trying to lock already locked HWSEM */
		zassert_false(hwsem_lock(devices[device_idx], MASTER_ID),
			      "Unable to lock HWSEM %d\n", device_idx);
		/* Unlocking the locked HWSEM */
		zassert_false(hwsem_unlock(devices[device_idx], MASTER_ID),
			      "Unable to unlock HWSEM %d\n", device_idx);
		zassert_false(hwsem_unlock(devices[device_idx], MASTER_ID),
			      "Unable to unlock HWSEM %d\n", device_idx);
	}
}

/* Test Case to lock single HWSEM Node using trylock API */
ZTEST(hwsem_basic, test_trylock)
{
	/* Use only the single HWSEM device instance for this test */
	const struct device *device = devices[0];

	/* First trylock: can return 0 (success) or -EBUSY (busy, locked by another core) */
	int ret1 = hwsem_trylock(device, MASTER_ID);

	zassert_true(ret1 == 0 || ret1 == -EBUSY,
		     "Unexpected return from first hwsem_trylock: %d", ret1);

	if (ret1 == 0) {
		/* Unlock if the HWSEM is locked */
		zassert_false(hwsem_unlock(device, MASTER_ID),
			      "Unable to unlock HWSEM");
	}
}

/* Test case to unlock a non-locked HWSEM */
ZTEST(hwsem_basic, test_unlock)
{
	/* Use only the single HWSEM device instance for this test */
	const struct device *device = devices[0];

	int ret = hwsem_unlock(device, MASTER_ID);

	zassert_equal(ret, -1,
		      "Error occurred while trying to unlock; returned %d\n", ret);
}

/*
 * Test case to ensure multiple cores acquire the hardware semaphore to claim
 * the ownership of a shared resource (LED).
 * The core that acquires the semaphore toggles the LED before releasing it.
 * Each core repeats the above process.
 */
ZTEST(hwsem_shared_peripheral, hwsem0_sharing_led)
{
	int iter;
	const struct device *device = devices[0];

	zassert_true(gpio_is_ready_dt(&led), "LED device not ready");

	zassert_equal(gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE), 0,
		      "Unable to configure the LED");

	for (iter = 0; iter < SHARED_TEST_ITERATIONS; iter++) {
		zassert_false(hwsem_lock(device, MASTER_ID),
			      "Unable to lock HWSEM\n");

		zassert_equal(gpio_pin_toggle_dt(&led), 0,
			      "Error while toggling the GPIO\n");

		k_msleep(SLEEP_TIME_MS);

		zassert_false(hwsem_unlock(device, MASTER_ID),
			      "Unable to unlock HWSEM\n");
	}
}

/* HWSEM Basic API Tests */
ZTEST_SUITE(hwsem_basic, NULL, NULL, NULL, NULL, NULL);

/* HWSEM Real-time test with shared peripheral */
ZTEST_SUITE(hwsem_shared_peripheral, NULL, NULL, NULL, NULL, NULL);

