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
 * @file benchmark.h
 *
 */

#ifndef BENCHMARK_H
#define BENCHMARK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define BENCHMARK_OK 0

typedef uint32_t (*exec_wrap_func_t)(void *arg);

typedef struct {
	exec_wrap_func_t wrapper_func;
	uint32_t execuion_time;
	uint32_t idle_time;
	uint32_t num_runs;
} benchmark_t;

typedef struct {
	uint32_t avg_time;
	float cpu_load;
} benchmark_result_t;

benchmark_t benchmark_create(exec_wrap_func_t func);

uint32_t benchmark_run_once(benchmark_t *benchmark, void *arg);

uint32_t benchmark_run_for(benchmark_t *benchmark, void *arg, uint32_t num_runs);

benchmark_result_t benchmark_summarize(const benchmark_t *benchmark);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /* BENCHMARK_H */
