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
 * @file utmr.c
 */

#include "utmr.h"
#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(utimer_app, CONFIG_LOG_DEFAULT_LEVEL);

#define MHZ (1000000)

#define UTIMER_FREQ 400 * MHZ

#define UTIMER DT_CHILD(DT_NODELABEL(utimer0), counter)

static void utimer_callback(const struct device *dev, void *user_data);
static uint32_t utimer_ticks(void);
static uint32_t utimer_nanoseconds(void);
static uint32_t utimer_microseconds(void);
static uint32_t utimer_milliseconds(void);

static const struct device *counter_dev;
static uint32_t seconds;

void utimer_init(void)
{
	counter_dev = DEVICE_DT_GET(UTIMER);

	if (!device_is_ready(counter_dev)) {
		LOG_ERR("utimer device not ready");
		return;
	}

	const struct counter_top_cfg cfg = {.ticks = UTIMER_FREQ,
					    .callback = utimer_callback,
					    .flags = COUNTER_CONFIG_INFO_COUNT_UP,
					    .user_data = NULL};

	counter_set_top_value(counter_dev, &cfg);
}

void utimer_start(void)
{
	seconds = 0;

	if (counter_start(counter_dev)) {
		LOG_ERR("Failed to start utimer");
	}
}

void utimer_stop(void)
{
	if (counter_stop(counter_dev)) {
		LOG_ERR("Failed to stop utimer");
	}
}

uint32_t utimer_get_s(void)
{
	return seconds;
}

uint32_t utimer_get_ms(void)
{
	return seconds * 1000 + utimer_milliseconds();
}

uint32_t utimer_get_us(void)
{
	return seconds * 1000000 + utimer_microseconds();
}

uint64_t utimer_get_ns(void)
{
	return (uint64_t)seconds * 1000000000 + utimer_nanoseconds();
}

#pragma GCC push_options
#pragma GCC optimize("O0")
static void utimer_callback(const struct device *dev, void *user_data)
{
	(void)dev, (void)user_data;

	++seconds;
}
#pragma GCC pop_options

static uint32_t utimer_ticks(void)
{
	uint32_t cntr;

	if (counter_get_value(counter_dev, &cntr)) {
		LOG_ERR("Failed to get utimer ticks");
		return 0;
	}
	return cntr;
}

static uint32_t utimer_nanoseconds(void)
{
	return utimer_ticks() * 5 / 2;
}

static uint32_t utimer_microseconds(void)
{
	return utimer_ticks() / 400;
}

static uint32_t utimer_milliseconds(void)
{
	return utimer_ticks() / 400000;
}
