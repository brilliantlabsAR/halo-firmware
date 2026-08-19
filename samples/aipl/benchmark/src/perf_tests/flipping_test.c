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
 * @file flipping_test.c
 */

#include "perf_tests.h"
#include "aipl_flip.h"

static uint32_t flipping_wrapper(void *arg);

benchmark_t create_flipping_benchmark(void)
{
	return benchmark_create(&flipping_wrapper);
}

static uint32_t flipping_wrapper(void *arg)
{
	flip_op_arg_t *args = (flip_op_arg_t *)arg;

	return aipl_flip_img(args->src, args->dst, args->horizontal, args->vertical);
}
