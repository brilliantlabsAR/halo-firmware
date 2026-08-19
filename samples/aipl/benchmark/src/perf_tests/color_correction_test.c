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
 * @file color_correction_test.c
 */

#include "perf_tests.h"
#include "aipl_color_correction.h"

static uint32_t color_correction_wrapper(void *arg);

benchmark_t create_color_correction_benchmark(void)
{
	return benchmark_create(&color_correction_wrapper);
}

static uint32_t color_correction_wrapper(void *arg)
{
	cc_op_arg_t *args = (cc_op_arg_t *)arg;

	return aipl_color_correction_rgb_img(args->src, args->dst, args->ccm);
}
