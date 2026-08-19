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
 * @file fps_counter.c
 *
 */

#include "fps_counter.h"
#include "utmr.h"
#include <stdlib.h>

fps_counter_t fps_counter_create(uint32_t interval_duration_us)
{
	fps_counter_t counter = {NULL, 0, interval_duration_us, utimer_get_us()};

	return counter;
}

void fps_counter_destroy(fps_counter_t *counter)
{
	fps_counter_interval_t *interval = counter->intervals;

	while (interval != NULL) {
		fps_counter_interval_t *prev = interval->prev;

		free(interval);

		interval = prev;
	}
}

void fps_counter_reset(fps_counter_t *counter, uint32_t interval_duration_us)
{
	fps_counter_destroy(counter);

	counter->intervals = NULL;
	counter->intervals_num = 0;
	counter->interval_duration_us = interval_duration_us;
	counter->start_timestamp_us = utimer_get_us();
}

void fps_counter_add_frame(fps_counter_t *counter)
{
	uint32_t timestamp = utimer_get_us();
	uint32_t curr_interval =
		(timestamp - counter->start_timestamp_us) / counter->interval_duration_us;

	if (curr_interval < counter->intervals_num) {
		++counter->intervals->frames;
	} else {
		while (curr_interval >= counter->intervals_num) {
			fps_counter_interval_t *new_interval =
				(fps_counter_interval_t *)malloc(sizeof(fps_counter_interval_t));

			new_interval->frames = 0;
			new_interval->prev = counter->intervals;

			counter->intervals = new_interval;

			++counter->intervals_num;
		}

		counter->intervals->frames = 1;
	}
}

float fps_counter_get_average(const fps_counter_t *counter)
{
	if (counter->intervals_num == 0) {
		return 0.0f;
	}

	uint32_t sum = 0;

	fps_counter_interval_t *interval = counter->intervals;

	while (interval != NULL) {
		sum += interval->frames;

		interval = interval->prev;
	}

	return 1000000.0f / counter->interval_duration_us * sum / counter->intervals_num;
}
