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
 * @file fps_counter.h
 *
 */

#ifndef FPS_COUNTER_H
#define FPS_COUNTER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct fps_counter_interval fps_counter_interval_t;
struct fps_counter_interval {
	uint32_t frames;
	fps_counter_interval_t *prev;
};

typedef struct {
	fps_counter_interval_t *intervals;
	uint32_t intervals_num;
	uint32_t interval_duration_us;
	uint32_t start_timestamp_us;
} fps_counter_t;

fps_counter_t fps_counter_create(uint32_t interval_duration_us);

void fps_counter_destroy(fps_counter_t *counter);

void fps_counter_reset(fps_counter_t *counter, uint32_t interval_duration_us);

void fps_counter_add_frame(fps_counter_t *counter);

float fps_counter_get_average(const fps_counter_t *counter);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*FPS_COUNTER_H*/
