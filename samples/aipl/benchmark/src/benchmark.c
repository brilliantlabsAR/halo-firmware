/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 *
 */

/**
 * @file benchmark.c
 *
 */

#include "benchmark.h"
#include "utmr.h"
#include "cpu_usage.h"
#include <zephyr/sys/printk.h>

benchmark_t benchmark_create(exec_wrap_func_t func)
{
	benchmark_t benchmark = {func, 0, 0, 0};

	return benchmark;
}

uint32_t benchmark_run_once(benchmark_t *benchmark, void *arg)
{
	uint32_t now = utimer_get_us();

	zephyr_get_idle_time();

	uint32_t res = benchmark->wrapper_func(arg);

	uint32_t elapsed = utimer_get_us() - now;

	benchmark->execuion_time += elapsed;
	benchmark->idle_time += zephyr_get_idle_time();
	++benchmark->num_runs;

	return res;
}

uint32_t benchmark_run_for(benchmark_t *benchmark, void *arg, uint32_t num_runs)
{
	uint32_t now = utimer_get_us();

	zephyr_get_idle_time();

	for (uint32_t i = 0; i < num_runs; ++i) {
		uint32_t res = benchmark->wrapper_func(arg);

		if (res != BENCHMARK_OK) {
			return res;
		}
	}

	uint32_t elapsed = utimer_get_us() - now;

	benchmark->execuion_time += elapsed;
	benchmark->idle_time += zephyr_get_idle_time();
	benchmark->num_runs += num_runs;

	return BENCHMARK_OK;
}

benchmark_result_t benchmark_summarize(const benchmark_t *benchmark)
{
	benchmark_result_t res = {0};

	res.avg_time = benchmark->execuion_time / benchmark->num_runs;
	res.cpu_load =
		(float)(benchmark->execuion_time - benchmark->idle_time) / benchmark->execuion_time;

	return res;
}
