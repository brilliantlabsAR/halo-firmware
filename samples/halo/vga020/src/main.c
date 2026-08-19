/* Copyright (c) 2019 Jan Van Winkel <jan.van_winkel@dxplore.eu>
 *
 * Based on ST7789V sample:
 * Copyright (c) 2019 Marc Reilly
 *
 * Copyright 2024 Alif Semiconductor
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/display/cdc200.h>
#ifdef CONFIG_MIPI_DSI
#include <zephyr/drivers/mipi_dsi/dsi_dw.h>
#endif /* CONFIG_MIPI_DSI */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(disp, LOG_LEVEL_INF);

#include <canvas.h>

Color red = COLOR_HEX(0xFF0000);
Color green = COLOR_HEX(0x00FF00);
Color blue = COLOR_HEX(0x0000FF);
Color yellow = COLOR_HEX(0xFFFF00);
Color cyan = COLOR_HEX(0x00FFFF);
Color white = COLOR_HEX(0xFFFFFF);

#include "bitmap.h"
int main(void)
{

	struct cdc200_display_caps capabilities;
	struct cdc200_fb_desc layer = {0};
	const struct device *display_dev;

	struct display_capabilities panel_caps;
	const struct device *panel;
	const struct device *dsi;
	uint32_t start, end;
	int ret;

	panel = DEVICE_DT_GET(DT_CHOSEN(zephyr_panel));
	if (!device_is_ready(panel)) {
		LOG_ERR("Device %s not found. Aborting sample.", panel->name);
		return -1;
	}

	dsi = DEVICE_DT_GET(DT_ALIAS(mipi_dsi));
	if (!device_is_ready(dsi)) {
		LOG_ERR("Device %s not found. Aborting sample.", dsi->name);
		return -1;
	}

	LOG_INF("Enable Ensemble-DSI Device video mode.");
	ret = dsi_dw_set_mode(dsi, DSI_DW_VIDEO_MODE);
	if (ret) {
		LOG_ERR("DSI Host controller set to video mode.");
		return -1;
	}

	display_get_capabilities(panel, &panel_caps);
	LOG_INF("Panel Orientation - %d", panel_caps.current_orientation);

	display_blanking_off(panel);

	display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display_dev)) {
		LOG_ERR("Device %s not found. Aborting sample.", display_dev->name);
		return -1;
	}

	LOG_INF("Display sample for %s", display_dev->name);
	LOG_INF("Enabling CDC200 Device.");
	cdc200_set_enable(display_dev, true);
	cdc200_get_capabilities(display_dev, &capabilities);

	LOG_INF("Display Capabilities");
	LOG_INF("Panel resolution, supported formats - (%d, %d), %d",
		capabilities.x_panel_resolution, capabilities.y_panel_resolution,
		capabilities.supported_pixel_formats);
	LOG_INF("CDC200 orientation - %d", capabilities.current_orientation);

	for (int i = 0; i <= 1; i++) {
		LOG_INF("Display Capabilities layer %d:", i + 1);
		LOG_INF("\tlayer_enabled - %d", capabilities.layer[i].layer_en);
		LOG_INF("\t(x_res, y_res) - (%d, %d)", capabilities.layer[i].x_resolution,
			capabilities.layer[i].y_resolution);
		LOG_INF("\tcurr_pix_fmt - %d", capabilities.layer[i].current_pixel_format);
	}

	if (capabilities.layer[0].layer_en) {
		cdc200_get_framebuffer(display_dev, 0, &layer);
		LOG_INF("FB0 - 0x%08x, size - %d", (uint32_t)layer.fb_addr, layer.fb_size);
		Canvas canvas;
		canvas_init(&canvas, (uint8_t (*)[240][3])layer.fb_addr);

		canvas_clear(&canvas, COLOR_HEX(0x000000));

		start = k_uptime_get_32();
		canvas_draw_rect(&canvas, 50, 50, 100, 80, green, false);
		end = k_uptime_get_32();
		LOG_INF("draw rect time: %d ms", end - start);

		start = k_uptime_get_32();
		canvas_draw_rect(&canvas, 200, 50, 80, 100, blue, true);
		end = k_uptime_get_32();
		LOG_INF("draw rect time: %d ms", end - start);

		int triangle[] = {120, 30, 180, 100, 60, 100};
		canvas_draw_polygon(&canvas, triangle, 3, white);
		end = k_uptime_get_32();
		LOG_INF("draw triangle time: %d ms", end - start);

		start = k_uptime_get_32();
		canvas_draw_bitmap(&canvas, 0, 0, 128, 64, gImage_ABC);
		end = k_uptime_get_32();
		LOG_INF("draw image time: %d ms", end - start);

		canvas_set_font(&canvas, &Dogica8px, 2);

		start = k_uptime_get_32();
		canvas_draw_char(&canvas, 'A', 0, 100, COLOR_HEX(0xFF0000));
		end = k_uptime_get_32();
		LOG_INF("draw char time: %d ms", end - start);

		start = k_uptime_get_32();
		canvas_draw_string(&canvas, "Hello World", 0, 20, COLOR_HEX(0x00FF00));
		end = k_uptime_get_32();
		LOG_INF("draw string time: %d ms", end - start);

		start = k_uptime_get_32();
		canvas_draw_line(&canvas, 10, 10, 100, 10, red);
		end = k_uptime_get_32();
		LOG_INF("draw line time: %d ms", end - start);

		uint8_t brightness = 0;
		while (1) {
			printk("Brightness: %d\n", brightness);

			display_set_brightness(panel, brightness);
			brightness += 1;
			if (brightness > 100) {
				brightness = 0;
			}
			k_msleep(1000);
		}
	}

	return 0;
}
