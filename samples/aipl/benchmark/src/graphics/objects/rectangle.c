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
 * @file rectangle.c
 */

#include <stdlib.h>
#include "objects.h"

typedef struct {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	d2_color color;
} graph_rectangle_t;

static void graph_rectangle_draw(uint32_t format, void *data);
static void graph_rectangle_destroy(graph_object_t *rect);

graph_object_t graph_create_rectangle(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
				      d2_color color)
{
	graph_rectangle_t *rect = (graph_rectangle_t *)malloc(sizeof(graph_rectangle_t));

	rect->x = x;
	rect->y = y;
	rect->width = width;
	rect->height = height;
	rect->color = color;

	graph_object_t object = {graph_rectangle_draw, 0, rect, graph_rectangle_destroy};

	return object;
}

static void graph_rectangle_draw(uint32_t format, void *data)
{
	(void)format;

	graph_rectangle_t *rect = (graph_rectangle_t *)data;

	d2_device *handle = aipl_dave2d_handle();

	d2_cliprect(handle, (d2_border)rect->x, (d2_border)rect->y,
		    (d2_border)rect->x + rect->width, (d2_border)rect->y + rect->height);

	d2_setfillmode(handle, d2_fm_color);
	d2_setcolor(handle, 0, rect->color);
	d2_setalpha(handle, 0xFF);

	d2_renderbox(handle, (d2_point)D2_FIX4(rect->x), (d2_point)D2_FIX4(rect->y),
		     (d2_point)D2_FIX4(rect->width), (d2_point)D2_FIX4(rect->height));
}

static void graph_rectangle_destroy(graph_object_t *rect)
{
	free(rect->data);
}
