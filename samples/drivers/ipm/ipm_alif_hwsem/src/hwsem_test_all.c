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
#define HWSEM_NUM 16

#if defined(CONFIG_SOC_E3_DK_RTSS_HP) || \
defined(CONFIG_SOC_E7_DK_RTSS_HP)
#define MASTER_ID 0xF00DF00D
#elif defined(CONFIG_SOC_E3_DK_RTSS_HE) || \
defined(CONFIG_SOC_E7_DK_RTSS_HE)
#define MASTER_ID 0xC0DEC0DE
#endif

const struct device *hwsem[16];

int main(void)
{
	int i = 0;

	LOG_INF("Test all 16 Hardware Semaphores(HWSEM) on %s\n", CONFIG_BOARD);
	hwsem[0] = DEVICE_DT_GET(DT_NODELABEL(hwsem0));
	hwsem[1] = DEVICE_DT_GET(DT_NODELABEL(hwsem1));
	hwsem[2] = DEVICE_DT_GET(DT_NODELABEL(hwsem2));
	hwsem[3] = DEVICE_DT_GET(DT_NODELABEL(hwsem3));
	hwsem[4] = DEVICE_DT_GET(DT_NODELABEL(hwsem4));
	hwsem[5] = DEVICE_DT_GET(DT_NODELABEL(hwsem5));
	hwsem[6] = DEVICE_DT_GET(DT_NODELABEL(hwsem6));
	hwsem[7] = DEVICE_DT_GET(DT_NODELABEL(hwsem7));
	hwsem[8] = DEVICE_DT_GET(DT_NODELABEL(hwsem8));
	hwsem[9] = DEVICE_DT_GET(DT_NODELABEL(hwsem9));
	hwsem[10] = DEVICE_DT_GET(DT_NODELABEL(hwsem10));
	hwsem[11] = DEVICE_DT_GET(DT_NODELABEL(hwsem11));
	hwsem[12] = DEVICE_DT_GET(DT_NODELABEL(hwsem12));
	hwsem[13] = DEVICE_DT_GET(DT_NODELABEL(hwsem13));
	hwsem[14] = DEVICE_DT_GET(DT_NODELABEL(hwsem14));
	hwsem[15] = DEVICE_DT_GET(DT_NODELABEL(hwsem15));
	for (i = 0; i < HWSEM_NUM ; ++i) {
		if (hwsem_trylock(hwsem[i], MASTER_ID)) {
			LOG_ERR("Unable to lock HWSEM%d\n", i);
			return -1;
		}
		LOG_INF("Locked HWSEM%d!\n", i);
		LOG_INF("Perform critical work here 1 !!!!\n");

		if (hwsem_trylock(hwsem[i], MASTER_ID)) {
			LOG_ERR("Unable to lock HWSEM%d\n", i);
			hwsem_unlock(hwsem[i], MASTER_ID);
			return -1;
		}
		LOG_INF("Locked HWSEM%d!\n", i);
		LOG_INF("Perform critical work here 2 !!!!\n");

		if (hwsem_unlock(hwsem[i], MASTER_ID)) {
			LOG_ERR("Unable to unlock HWSEM%d\n", i);
			return -1;
		}
		LOG_INF("Unlocked HWSEM%d!\n", i);

		if (hwsem_unlock(hwsem[i], MASTER_ID)) {
			LOG_ERR("Unable to unlock HWSEM%d\n", i);
			return -1;
		}
		LOG_INF("Unlocked HWSEM%d!\n", i);
	}
	return 0;
}
