/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/comparator/alif_cmp.h>
#include <zephyr/drivers/gpio.h>
#include <stdint.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ALIF_CMP);

#define NODE_LABEL DT_COMPAT_GET_ANY_STATUS_OKAY(alif_cmp)

/* Marcos for call back */
volatile uint8_t call_back_event;
volatile uint8_t cmp_status;

void cmp_callback(const struct device *dev, uint8_t status)
{
	call_back_event = 1;
	cmp_status = status;
}

int main(void)
{
	uint32_t loop = 10;
	uint8_t cmp = DT_ENUM_IDX(NODE_LABEL, driver_instance);

	static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_NODELABEL(aled0), gpios);

	if (!device_is_ready(led.port)) {
		printk("led device not ready\n");
		return -1;
	}

	if (cmp) {
		int ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_LOW);

		if (ret != 0) {
			printk("Error %d: failed to configure LED pin\n", ret);
			return -1;
		}

		k_msleep(2000);
	}

	const struct device *const cmp_dev = DEVICE_DT_GET(NODE_LABEL);

	if (!device_is_ready(cmp_dev)) {
		printk("device not ready\n");
		return -1;
	}

	struct cmp_params cmp_para = {
		.filter_taps = 5,
		.prescalar = 8,
		.polarity = 0,
		.callback = cmp_callback,
	};

	cmp_configure(cmp_dev, &cmp_para);

	LOG_INF("start comparing");

	cmp_start_compare(cmp_dev);

	while (loop--) {

		if (cmp) {

			gpio_pin_toggle_dt(&led);
		}

		/* wait for the comparasion */
		while (call_back_event == 0) {
			;
		}

		call_back_event = 0;

		if (cmp) {

			/* Introducing a delay to stabilize input
			 * voltage for comparator measurement
			 */
			k_msleep(50);

			/* If user give +ve input voltage more than -ve input voltage,
			 * status will be set to 1.
			 */
			if (cmp_status == 1) {
				LOG_INF("positive input voltage is greater than negative input"
					 " voltage");
			}
			/* If user give -ve input voltage more than +ve input voltage,
			 * status will be set to 0.
			 */
			else if (cmp_status == 0) {
				LOG_INF("negative input voltage is greater than the positive input"
					" voltage");
			} else {
				LOG_INF("ERROR: Status detection is failed");
			}
		}
	}

	LOG_INF("Comparison Completed");

	return 0;
}
