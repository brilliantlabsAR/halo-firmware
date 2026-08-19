/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 *
 */

#include "cpu_usage.h"
#include <string.h>
#include <stdbool.h>
#include <zephyr/kernel.h>
#include "utmr.h"

static bool cpu_usage_enabled;
static volatile uint32_t idle_time_sum;
static volatile uint32_t task_switch_timestamp;
static volatile bool idle_task_running;

void sys_trace_thread_switched_in_user(void)
{
	if (!cpu_usage_enabled) {
		return;
	}

	if (strcmp(k_thread_name_get(k_current_get()), "idle") == 0) {
		idle_task_running = true;
	} else {
		idle_task_running = false;
	}
	task_switch_timestamp = utimer_get_us();
}

void sys_trace_thread_switched_out_user(void)
{
	if (!cpu_usage_enabled) {
		return;
	}

	if (idle_task_running) {
		idle_time_sum += utimer_get_us() - task_switch_timestamp;
	}
}

void cpu_usage_enable(void)
{
	cpu_usage_enabled = true;
}

uint32_t zephyr_get_idle_time(void)
{
	uint32_t tmp = idle_time_sum;

	idle_time_sum = 0;

	return tmp;
}
