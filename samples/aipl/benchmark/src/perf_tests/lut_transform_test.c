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
 * @file lut_transform_test.c
 */

#include "perf_tests.h"
#include "aipl_lut_transform.h"

static uint32_t lut_transform_wrapper(void *arg);

benchmark_t create_lut_transform_benchmark(void)
{
	return benchmark_create(&lut_transform_wrapper);
}

static uint32_t lut_transform_wrapper(void *arg)
{
	gc_op_arg_t *args = (gc_op_arg_t *)arg;

	return aipl_lut_transform_rgb_img(args->src, args->dst, args->gamma_lut);
}
