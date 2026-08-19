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
 * @file white_balance_test.c
 */

#include "perf_tests.h"
#include "aipl_white_balance.h"

static uint32_t white_balance_wrapper(void *arg);

benchmark_t create_white_balance_benchmark(void)
{
	return benchmark_create(&white_balance_wrapper);
}

static uint32_t white_balance_wrapper(void *arg)
{
	wb_op_arg_t *args = (wb_op_arg_t *)arg;

	return aipl_white_balance_rgb_img(args->src, args->dst, args->ar, args->ag, args->ab);
}
