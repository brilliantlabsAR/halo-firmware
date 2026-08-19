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
 * @file rotation_test.c
 */

#include "perf_tests.h"
#include "aipl_rotate.h"

static uint32_t rotation_wrapper(void *arg);

benchmark_t create_rotation_benchmark(void)
{
	return benchmark_create(&rotation_wrapper);
}

static uint32_t rotation_wrapper(void *arg)
{
	rot_op_arg_t *args = (rot_op_arg_t *)arg;

	return aipl_rotate_img(args->src, args->dst, args->rotation);
}
