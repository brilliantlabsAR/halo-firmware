/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 *
 */
#include <zephyr/kernel.h>
#include <string.h>
#include <zephyr/drivers/hwsem_ipm.h>
#include <zephyr/logging/log.h>

#define LOG_MODULE_NAME alif_hwsem_log
LOG_MODULE_REGISTER(LOG_MODULE_NAME);
const struct device *hwsem0;

#if defined(CONFIG_SOC_E3_DK_RTSS_HP) || \
defined(CONFIG_SOC_E7_DK_RTSS_HP)
#define MASTER_ID 0xF00DF00D
#elif defined(CONFIG_SOC_E3_DK_RTSS_HE) || \
defined(CONFIG_SOC_E7_DK_RTSS_HE)
#define MASTER_ID 0xC0DEC0DE
#endif

int main(void)
{
	LOG_INF("Hardware Semaphore (HWSEM) example on %s\n", CONFIG_BOARD);
	hwsem0 = DEVICE_DT_GET(DT_NODELABEL(hwsem0));
	if (!device_is_ready(hwsem0)) {
		LOG_ERR("HWSEM0 not ready\n");
		return -1;
	}


	if (hwsem_lock(hwsem0, MASTER_ID)) {
		LOG_ERR("Unable to lock HWSEM0\n");
		return -1;
	}

	LOG_INF("Locked HWSEM0!\n");
	LOG_INF("Perform critical work here 1 !!!!\n");

	if (hwsem_lock(hwsem0, MASTER_ID)) {
		LOG_ERR("Unable to lock HWSEM0\n");
		hwsem_unlock(hwsem0, MASTER_ID);
		return -1;
	}
	LOG_INF("Locked HWSEM0!\n");
	LOG_INF("Perform critical work here 2 !!!!\n");

	if (hwsem_unlock(hwsem0, MASTER_ID)) {
		LOG_ERR("Unable to unlock HWSEM0\n");
		return -1;
	}
	LOG_INF("Unlocked HWSEM0!\n");

	if (hwsem_unlock(hwsem0, MASTER_ID)) {
		LOG_ERR("Unable to unlock HWSEM0\n");
		return -1;
	}
	LOG_INF("Unlocked HWSEM0!\n");

	return 0;
}
