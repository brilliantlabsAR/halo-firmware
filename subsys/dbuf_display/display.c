/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 *
 */

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/display/cdc200.h>
#include <zephyr/drivers/mipi_dsi/dsi_dw.h>
#include <zephyr/logging/log.h>
#include "display.h"

LOG_MODULE_REGISTER(display_app, LOG_LEVEL_DBG);

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)

#define DISPLAY_WIDTH  DT_PROP(DISPLAY_NODE, width)
#define DISPLAY_HEIGHT DT_PROP(DISPLAY_NODE, height)

#define BUFFER_SIZE (DISPLAY_WIDTH * DISPLAY_HEIGHT * 2)

#ifdef CONFIG_DBUF_DISPLAY_SECTION
#define DBUF_DISPLAY_ATTRS __attribute__((section(CONFIG_DBUF_DISPLAY_SECTION)))
#else
#define DBUF_DISPLAY_ATTRS
#endif

uint8_t DBUF_DISPLAY_ATTRS buf0[BUFFER_SIZE];
uint8_t DBUF_DISPLAY_ATTRS buf1[BUFFER_SIZE];

enum {
	BUFFER_1 = 0,
	BUFFER_2 = 1,
	NUM_BUFFERS
};

static uint8_t *buffers[NUM_BUFFERS] = {buf0, buf1};

static uint8_t current_buffer = BUFFER_1;
static uint32_t frame_durations[NUM_BUFFERS] = {1, 1};
static uint32_t switch_times[NUM_BUFFERS] = {0, 0};

static const struct device *display_dev = DEVICE_DT_GET(DISPLAY_NODE);

/* Main Display Initialization Function */
int display_init(void)
{
	if (!device_is_ready(display_dev)) {
		LOG_ERR("Device %s not found. Aborting sample.", display_dev->name);
		return -ENODEV;
	}

	const struct device *panel = DEVICE_DT_GET(DT_ALIAS(panel));
	const struct device *dsi = DEVICE_DT_GET(DT_ALIAS(mipi_dsi));
	int ret;

	if (!device_is_ready(panel)) {
		LOG_ERR("Device %s not found. Aborting sample.", panel->name);
		return -ENODEV;
	}

	if (!device_is_ready(dsi)) {
		LOG_ERR("Device %s not found. Aborting sample.", dsi->name);
		return -ENODEV;
	}

	LOG_INF("Enabling Ensemble-DSI Device video mode.");
	ret = dsi_dw_set_mode(dsi, DSI_DW_VIDEO_MODE);
	if (ret) {
		LOG_ERR("Failed to set DSI Host controller to video mode.");
		return ret;
	}

	cdc200_set_enable(display_dev, true);

	return 0;
}

void display_set_next_frame_duration(uint32_t duration)
{
	frame_durations[current_buffer] = duration;
}

void display_next_frame(void)
{
	uint32_t switch_time = switch_times[current_buffer];

	uint32_t current_frame_time = k_cyc_to_ms_floor32(k_cycle_get_32() - switch_time);

	if (current_frame_time < frame_durations[current_buffer]) {
		k_sleep(K_MSEC(frame_durations[current_buffer] - current_frame_time));
	}

	current_buffer = (current_buffer + 1) % NUM_BUFFERS;

	struct display_buffer_descriptor desc = {.buf_size = BUFFER_SIZE,
						 .width = DISPLAY_WIDTH,
						 .height = DISPLAY_HEIGHT,
						 .pitch = DISPLAY_WIDTH};

	display_write(display_dev, 0, 0, &desc, buffers[current_buffer]);

	switch_times[current_buffer] = k_cycle_get_32();
}

void *display_active_buffer(void)
{
	return buffers[current_buffer];
}

void *display_inactive_buffer(void)
{
	return buffers[(current_buffer + 1) % NUM_BUFFERS];
}

uint32_t display_width(void)
{
	return DISPLAY_WIDTH;
}

uint32_t display_height(void)
{
	return DISPLAY_HEIGHT;
}
