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
 * @file draw_object_test.c
 */

#include "perf_tests.h"
#include "graphics.h"
#include "aipl_dave2d.h"

static uint32_t draw_object_wrapper(void *arg);

benchmark_t create_draw_object_benchmark(void)
{
	return benchmark_create(&draw_object_wrapper);
}

static uint32_t draw_object_wrapper(void *arg)
{
	graph_object_t *obj = (graph_object_t *)arg;

	graph_draw_object(obj);

	d2_device *handle = aipl_dave2d_handle();

	d2_endframe(handle);

	return 0;
}
