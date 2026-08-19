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
 * @file resize_test.c
 */

#include "perf_tests.h"
#include "aipl_resize.h"

static uint32_t resize_wrapper(void *arg);

benchmark_t create_resize_benchmark(void)
{
	return benchmark_create(&resize_wrapper);
}

static uint32_t resize_wrapper(void *arg)
{
	op_arg_t *args = (op_arg_t *)arg;

	return aipl_resize_img(args->src, args->dst, true);
}
