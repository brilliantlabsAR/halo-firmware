/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>

LOG_MODULE_REGISTER(main);

#define SI570_NODE DT_ALIAS(clock_si570)
const struct device *si570_dev = DEVICE_DT_GET(SI570_NODE);

int main(void)
{
	LOG_INF("SI570 demo starting");

	if (!device_is_ready(si570_dev)) {
		LOG_ERR("SI570 device is not ready");
		return -1;
	}

	uint64_t frequency_list[] = {
		20000000ULL, 21000000ULL, 22000000ULL, 23000000ULL, 24000000ULL,
		25000000ULL, 25000100ULL, 25000000ULL, 24000900ULL, 25000000ULL,
	};

	while (1) {
		/* Repeatedly cycle through the frequency list */
		for (uint32_t i = 0; i < ARRAY_SIZE(frequency_list); i++) {
			LOG_INF("Setting frequency of %llu Hz", frequency_list[i]);

			int ret = clock_control_set_rate(si570_dev, NULL, &frequency_list[i]);

			if (ret) {
				LOG_ERR("Failed to set clock rate, err %d", ret);
				return ret;
			}

			uint32_t rate_get;

			ret = clock_control_get_rate(si570_dev, NULL, &rate_get);
			if (ret) {
				LOG_ERR("Failed to get clock rate, err %d", ret);
				return ret;
			}

			LOG_INF("read back frequency of %u Hz", rate_get);

			k_sleep(K_SECONDS(5));
		}
	}
}
