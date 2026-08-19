/* Copyright (c) 2019 Jan Van Winkel <jan.van_winkel@dxplore.eu>
 *
 * Based on ST7789V sample:
 * Copyright (c) 2019 Marc Reilly
 *
 * Copyright 2024 Alif Semiconductor
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(disp, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/display/cdc200.h>
#ifdef CONFIG_MIPI_DSI
#include <zephyr/drivers/mipi_dsi/dsi_dw.h>
#endif /* CONFIG_MIPI_DSI */
#include "alif_logo.h"

#define RED_ARGB8888	0x00ff0000
#define GREEN_ARGB8888	0x0000ff00
#define BLUE_ARGB8888	0x000000ff
#define RED_RGB888	0x00ff0000
#define GREEN_RGB888	0x0000ff00
#define BLUE_RGB888	0x000000ff
#define RED_RGB565	0xf800
#define GREEN_RGB565	0x07e0
#define BLUE_RGB565	0x001f

#define CDC200_PIXEL_SIZE_ARGB8888	4
#define CDC200_PIXEL_SIZE_RGB888	3
#define CDC200_PIXEL_SIZE_RGB565	2

enum corner {
	TOP_LEFT,
	TOP_RIGHT,
	BOTTOM_RIGHT,
	BOTTOM_LEFT
};

typedef void (*fill_buffer)(enum corner corner, uint8_t grey, uint8_t *buf,
			    size_t buf_size);

#if (!defined(CONFIG_MIPI_DSI) || \
	!DT_NODE_HAS_PROP(DT_ALIAS(mipi_dsi), dpi_video_pattern_gen))
static void fill_buffer_argb8888(enum corner corner, uint8_t grey, uint8_t *buf,
				 size_t buf_size)
{
	uint32_t color = 0;

	switch (corner) {
	case TOP_LEFT:
		color = RED_ARGB8888;
		break;
	case TOP_RIGHT:
		color = GREEN_ARGB8888;
		break;
	case BOTTOM_RIGHT:
		color = BLUE_ARGB8888;
		break;
	case BOTTOM_LEFT:
		color = grey << 16 | grey << 8 | grey;
		break;
	}

	for (size_t idx = 0; idx < buf_size; idx += 4) {
		*((uint32_t *)(buf + idx)) = color;
	}
}

static void fill_buffer_rgb888(enum corner corner, uint8_t grey, uint8_t *buf,
			       size_t buf_size)
{
	uint32_t color = 0;

	switch (corner) {
	case TOP_LEFT:
		color = RED_RGB888;
		break;
	case TOP_RIGHT:
		color = GREEN_RGB888;
		break;
	case BOTTOM_RIGHT:
		color = BLUE_RGB888;
		break;
	case BOTTOM_LEFT:
		color = grey << 16 | grey << 8 | grey;
		break;
	}

	for (size_t idx = 0; idx < buf_size; idx += 3) {
		*(buf + idx + 2) = (color >> 16) & 0xff;
		*(buf + idx + 1) = (color >> 8) & 0xff;
		*(buf + idx + 0) = (color >> 0) & 0xff;
	}
}

static uint16_t get_rgb565_color(enum corner corner, uint8_t grey)
{
	uint16_t color = 0;

	switch (corner) {
	case TOP_LEFT:
		color = RED_RGB565;
		break;
	case TOP_RIGHT:
		color = GREEN_RGB565;
		break;
	case BOTTOM_RIGHT:
		color = BLUE_RGB565;
		break;
	case BOTTOM_LEFT:
		color = (grey & 0x1f) << 11 |
			(grey & 0x3f) << 5 | (grey & 0x1F);
		break;
	}
	return color;
}

static void fill_buffer_rgb565(enum corner corner, uint8_t grey, uint8_t *buf,
			       size_t buf_size)
{
	uint16_t color = get_rgb565_color(corner, grey);

	for (size_t idx = 0; idx < buf_size; idx += 2) {
		*(buf + idx + 1) = (color >> 8) & 0xFFu;
		*(buf + idx + 0) = (color >> 0) & 0xFFu;
	}
}

int get_pixel_size(enum display_pixel_format fmt)
{
	if (fmt == PIXEL_FORMAT_RGB_888)
		return CDC200_PIXEL_SIZE_RGB888;
	else if (fmt == PIXEL_FORMAT_ARGB_8888)
		return CDC200_PIXEL_SIZE_ARGB8888;
	else if (fmt == PIXEL_FORMAT_RGB_565)
		return CDC200_PIXEL_SIZE_RGB565;
	else
		return 0;
}
#endif /* (!defined(CONFIG_MIPI_DSI) || \
	* !DT_NODE_HAS_PROP(DT_ALIAS(mipi_dsi), dpi_video_pattern_gen))
	*/

int main(void)
{
#if (!defined(CONFIG_MIPI_DSI) || \
	!DT_NODE_HAS_PROP(DT_ALIAS(mipi_dsi), dpi_video_pattern_gen))
	struct display_buffer_descriptor buf_desc;
	struct cdc200_display_caps capabilities;
	struct cdc200_fb_desc fb_l2 = { 0 };
	fill_buffer fill_buffer_fnc = NULL;
	const struct device *display_dev;
	size_t pixel_size = 0;
	size_t buf_size = 0;
	uint8_t grey_count;
	size_t rect_w = 2;
	size_t rect_h = 1;
	uint8_t *buf;
	size_t scale;
	size_t x;
	size_t y;
#endif /* (!defined(CONFIG_MIPI_DSI) || \
	* !DT_NODE_HAS_PROP(DT_ALIAS(mipi_dsi), dpi_video_pattern_gen))
	*/

#if defined(CONFIG_MIPI_DSI)
	struct display_capabilities panel_caps;
	const struct device *panel;
	const struct device *dsi;
	int ret;

	panel = DEVICE_DT_GET(DT_ALIAS(panel));
	if (!device_is_ready(panel)) {
		LOG_ERR("Device %s not found. Aborting sample.",
			panel->name);
		return -1;
	}

	dsi = DEVICE_DT_GET(DT_ALIAS(mipi_dsi));
	if (!device_is_ready(dsi)) {
		LOG_ERR("Device %s not found. Aborting sample.",
			dsi->name);
		return -1;
	}

	LOG_INF("Rotating the display by 180 degrees");
	ret = display_set_orientation(panel, DISPLAY_ORIENTATION_ROTATED_180);
	if (ret == -ENOTSUP)
		LOG_INF("Un-supported Display Rotation.");

	LOG_INF("Enable Ensemble-DSI Device video mode.");
	ret = dsi_dw_set_mode(dsi, DSI_DW_VIDEO_MODE);
	if (ret) {
		LOG_ERR("DSI Host controller set to video mode.");
		return -1;
	}

	display_get_capabilities(panel, &panel_caps);
	LOG_INF("Panel Orientation - %d", panel_caps.current_orientation);

	display_blanking_off(panel);
#endif /* defined(CONFIG_MIPI_DSI) */

#if (!defined(CONFIG_MIPI_DSI) || \
	!DT_NODE_HAS_PROP(DT_ALIAS(mipi_dsi), dpi_video_pattern_gen))

	display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display_dev)) {
		LOG_ERR("Device %s not found. Aborting sample.",
			display_dev->name);
		return -1;
	}

	LOG_INF("Display sample for %s", display_dev->name);
	LOG_INF("Enabling CDC200 Device.");
	cdc200_set_enable(display_dev, true);
	cdc200_get_capabilities(display_dev, &capabilities);

	LOG_INF("Display Capabilities");
	LOG_INF("Panel resolution, supported formats - (%d, %d), %d",
			capabilities.x_panel_resolution,
			capabilities.y_panel_resolution,
			capabilities.supported_pixel_formats);
	LOG_INF("CDC200 orientation - %d",
			capabilities.current_orientation);

	for (int i = 0; i <= 1; i++) {
		LOG_INF("Display Capabilities layer %d:", i + 1);
		LOG_INF("\tlayer_enabled - %d",
				capabilities.layer[i].layer_en);
		LOG_INF("\t(x_res, y_res) - (%d, %d)",
				capabilities.layer[i].x_resolution,
				capabilities.layer[i].y_resolution);
		LOG_INF("\tcurr_pix_fmt - %d",
				capabilities.layer[i].current_pixel_format);
	}

	scale = (capabilities.layer[0].x_resolution / 8);
	rect_w *= scale;
	rect_h *= scale;
	buf_size = rect_w * rect_h;

	if (buf_size < (capabilities.layer[0].x_resolution)) {
		buf_size = capabilities.layer[0].x_resolution;
	}

	pixel_size =
		MAX(get_pixel_size(capabilities.layer[0].current_pixel_format),
		get_pixel_size(capabilities.layer[1].current_pixel_format));
	buf_size *= pixel_size;

	switch (capabilities.layer[0].current_pixel_format) {
	case PIXEL_FORMAT_ARGB_8888:
		fill_buffer_fnc = fill_buffer_argb8888;
		break;
	case PIXEL_FORMAT_RGB_888:
		fill_buffer_fnc = fill_buffer_rgb888;
		break;
	case PIXEL_FORMAT_RGB_565:
		fill_buffer_fnc = fill_buffer_rgb565;
		break;
	default:
		LOG_ERR("Unsupported pixel format. Aborting sample.");
		return -1;
	}

	buf = k_malloc(buf_size);
	if (buf == NULL) {
		LOG_ERR("Could not allocate memory."
			"Aborting sample. Required Heap Size - %d", buf_size);
		return -1;
	}

	if (capabilities.layer[1].layer_en) {
		cdc200_get_framebuffer(display_dev, 1, &fb_l2);
		memset((uint8_t *) fb_l2.fb_addr, 0, fb_l2.fb_size);
		memcpy((uint8_t *) fb_l2.fb_addr, logo, sizeof(logo));
	}

	if (capabilities.layer[0].layer_en) {
		cdc200_get_framebuffer(display_dev, 0, &fb_l2);
		LOG_INF("FB0 - 0x%08x, size - %d",
		(uint32_t)fb_l2.fb_addr, fb_l2.fb_size);
		(void)memset(buf, 0xFFu, buf_size);

		buf_desc.buf_size = buf_size;
		buf_desc.pitch = capabilities.layer[0].x_resolution;
		buf_desc.width = capabilities.layer[0].x_resolution;
		buf_desc.height = 1;

		for (int idx = 0;
		idx < capabilities.layer[0].y_resolution; idx += 1) {
			cdc200_display_write(display_dev, 0, 0,
					idx, &buf_desc, buf);
		}

		buf_desc.pitch = rect_w;
		buf_desc.width = rect_w;
		buf_desc.height = rect_h;

		fill_buffer_fnc(TOP_LEFT, 0, buf, buf_size);
		x = 0;
		y = 0;
		cdc200_display_write(display_dev, 0, x, y, &buf_desc, buf);

		fill_buffer_fnc(TOP_RIGHT, 0, buf, buf_size);
		x = capabilities.layer[0].x_resolution - rect_w;
		y = 0;
		cdc200_display_write(display_dev, 0, x, y, &buf_desc, buf);

		fill_buffer_fnc(BOTTOM_RIGHT, 0, buf, buf_size);
		x = capabilities.layer[0].x_resolution - rect_w;
		y = capabilities.layer[0].y_resolution - rect_h;
		cdc200_display_write(display_dev, 0, x, y, &buf_desc, buf);

		display_blanking_off(display_dev);

		grey_count = 0;
		x = 0;
		y = capabilities.layer[0].y_resolution - rect_h;

		while (1) {
			fill_buffer_fnc(BOTTOM_LEFT, grey_count,
					buf, buf_size);
			cdc200_display_write(display_dev, 0, x,
					     y, &buf_desc, buf);
			++grey_count;
			k_msleep(100);
		}
	}
#endif /* (!defined(CONFIG_MIPI_DSI) || \
	* !DT_NODE_HAS_PROP(DT_ALIAS(mipi_dsi), dpi_video_pattern_gen))
	*/
	return 0;
}
