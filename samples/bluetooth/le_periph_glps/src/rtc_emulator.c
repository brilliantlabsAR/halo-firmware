/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/kernel.h>
#include "prf_types.h"
#include "rtc_emulator.h"

/* Define a thread data structure */
struct k_thread rtc_thread_data;

/* Dummy timestamp info */
static prf_date_time_t device_base_time = {
	.year = 2024,
	.month = 1,
	.day = 1,
	.hour = 0,
	.min = 0,
	.sec = 0,
};

/* Define stack size for the thread */
#define STACK_SIZE 1024

/* Define the thread priority*/
#define THREAD_PRIORITY 7

/* Define a thread stack area */
K_THREAD_STACK_DEFINE(rtc_stack_area, STACK_SIZE);

/* Thread entry function */
void rtc_thread(void *arg1, void *arg2, void *arg3)
{
	while (1) {
		device_base_time.sec++;
		if (device_base_time.sec >= 60) {
			device_base_time.sec = 0;
			device_base_time.min++;
			if (device_base_time.min >= 60) {
				device_base_time.min = 0;
				device_base_time.hour++;
				if (device_base_time.hour >= 24) {
					device_base_time.hour = 0;
					device_base_time.day++;
					/*Simplified month length handling for demonstration
					 * purposes February and leap years are not handled
					 */
					if ((device_base_time.month == 1 ||
					     device_base_time.month == 3 ||
					     device_base_time.month == 5 ||
					     device_base_time.month == 7 ||
					     device_base_time.month == 8 ||
					     device_base_time.month == 10 ||
					     device_base_time.month == 12) &&
					    device_base_time.day > 31) {
						device_base_time.day = 1;
						device_base_time.month++;
					} else if ((device_base_time.month == 4 ||
						    device_base_time.month == 6 ||
						    device_base_time.month == 9 ||
						    device_base_time.month == 11) &&
						   device_base_time.day > 30) {
						device_base_time.day = 1;
						device_base_time.month++;
					} else if (device_base_time.month == 2 &&
						   device_base_time.day > 28) {
							/* Simplified, no
							 * leap year handling
							 */
						device_base_time.day = 1;
						device_base_time.month++;
					}
					if (device_base_time.month > 12) {
						device_base_time.month = 1;
						device_base_time.year++;
					}
				}
			}
		}
		k_sleep(K_SECONDS(1));
	}
}

void start_rtc_emulator(void)
{
	k_thread_create(&rtc_thread_data, rtc_stack_area, K_THREAD_STACK_SIZEOF(rtc_stack_area),
			rtc_thread, NULL, NULL, NULL, THREAD_PRIORITY, 0, K_NO_WAIT);
}

void *get_device_time(void)
{
	return &device_base_time;
}
