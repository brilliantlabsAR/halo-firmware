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
 * @file perf_tests.h
 *
 */

#ifndef PERF_TESTS_H
#define PERF_TESTS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "benchmark.h"
#include "aipl_image.h"
#include "aipl_rotate.h"

typedef struct {
	aipl_image_t *src;
	aipl_image_t *dst;
} op_arg_t;

typedef struct {
	aipl_image_t *src;
	aipl_image_t *dst;
	const float *ccm;
} cc_op_arg_t;

typedef struct {
	aipl_image_t *src;
	aipl_image_t *dst;
	float ar;
	float ag;
	float ab;
} wb_op_arg_t;

typedef struct {
	aipl_image_t *src;
	aipl_image_t *dst;
	uint8_t *gamma_lut;
} gc_op_arg_t;

typedef struct {
	aipl_image_t *src;
	aipl_image_t *dst;
	uint32_t left;
	uint32_t top;
	uint32_t right;
	uint32_t bottom;
} crop_op_arg_t;

typedef struct {
	aipl_image_t *src;
	aipl_image_t *dst;
	bool horizontal;
	bool vertical;
} flip_op_arg_t;

typedef struct {
	aipl_image_t *src;
	aipl_image_t *dst;
	aipl_rotation_t rotation;
} rot_op_arg_t;

benchmark_t create_draw_object_benchmark(void);

benchmark_t create_color_conversion_benchmark(void);

benchmark_t create_color_correction_benchmark(void);

benchmark_t create_white_balance_benchmark(void);

benchmark_t create_lut_transform_benchmark(void);

benchmark_t create_resize_benchmark(void);

benchmark_t create_cropping_benchmark(void);

benchmark_t create_flipping_benchmark(void);

benchmark_t create_rotation_benchmark(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /* PERF_TESTS_H */
