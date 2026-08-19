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
 * @file cropping_test.c
 */

#include "perf_tests.h"
#include "aipl_crop.h"

static uint32_t cropping_wrapper(void *arg);

benchmark_t create_cropping_benchmark(void)
{
	return benchmark_create(&cropping_wrapper);
}

static uint32_t cropping_wrapper(void *arg)
{
	crop_op_arg_t *args = (crop_op_arg_t *)arg;

	return aipl_crop_img(args->src, args->dst, args->left, args->top, args->right,
			     args->bottom);
}
