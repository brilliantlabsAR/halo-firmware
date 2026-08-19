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
 * @file image.c
 */

#include <stdlib.h>
#include <zephyr/cache.h>
#include "objects.h"
#include "aipl_dave2d.h"
#include "aipl_image.h"
#include "aipl_color_conversion.h"
#include "dbuf_display/display.h"

typedef struct {
	uint32_t x;
	uint32_t y;
	void *image;
	uint32_t pitch;
	uint32_t width;
	uint32_t height;
} graph_image_t;

static void dave2d_image_draw(uint32_t format, const graph_image_t *image);
static void graph_image_draw(uint32_t format, void *data);
static void graph_image_destroy(graph_object_t *image);

#ifdef CONFIG_IMG_CNV_SECTION
#define IMG_CNV_ATTRS __attribute__((section(CONFIG_IMG_CNV_SECTION)))
#else
#define IMG_CNV_ATTRS
#endif

static uint8_t IMG_CNV_ATTRS img_cnv_buff[DISPLAY_WIDTH * DISPLAY_HEIGHT * 4];

graph_object_t graph_create_image(uint32_t x, uint32_t y, void *image, uint32_t pitch,
				  uint32_t width, uint32_t height, d2_u32 format)
{
	graph_image_t *img = (graph_image_t *)malloc(sizeof(graph_image_t));

	img->x = x;
	img->y = y;
	img->image = image;
	img->pitch = pitch;
	img->width = width;
	img->height = height;

	graph_object_t object = {graph_image_draw, format, img, graph_image_destroy};

	return object;
}

static void dave2d_image_draw(uint32_t mode, const graph_image_t *image)
{
	sys_cache_data_flush_range(image->image,
			     image->pitch * image->height * aipl_dave2d_mode_px_size(mode));

	d2_device *handle = aipl_dave2d_handle();

	d2_cliprect(handle, (d2_border)image->x, (d2_border)image->y,
		    (d2_border)image->x + image->width - 1,
		    (d2_border)image->y + image->height - 1);

	d2_u8 alpha_mode = aipl_dave2d_mode_has_alpha(mode) ? d2_to_copy : d2_to_one;

	d2_settextureoperation(handle, alpha_mode, d2_to_copy, d2_to_copy, d2_to_copy);

	d2_settexture(handle, image->image, image->pitch, image->width, image->height, mode);

	d2_settexturemode(handle, d2_tm_filter);
	d2_setfillmode(handle, d2_fm_texture);
	d2_setblendmode(handle, d2_bm_alpha, d2_bm_one_minus_alpha);
	d2_setalphablendmode(handle, d2_bm_one, d2_bm_one_minus_alpha);

	d2_settexturemapping(handle, D2_FIX4(image->x), D2_FIX4(image->y), D2_FIX16(0), D2_FIX16(0),
			     D2_FIX16(1), D2_FIX16(0), D2_FIX16(0), D2_FIX16(1));

	d2_renderquad(handle, D2_FIX4(image->x), D2_FIX4(image->y),
		      D2_FIX4(image->x + image->width - 1), D2_FIX4(image->y),
		      D2_FIX4(image->x + image->width - 1), D2_FIX4(image->y + image->height - 1),
		      D2_FIX4(image->x), D2_FIX4(image->y + image->height - 1), 0);
}

static void graph_image_draw(uint32_t format, void *data)
{
	graph_image_t *image = (graph_image_t *)data;

	if (aipl_dave2d_format_supported(format)) {
		dave2d_image_draw(aipl_dave2d_format_to_mode(format), image);
	} else {
		graph_image_t converted = {.x = image->x,
					   .y = image->y,
					   .image = img_cnv_buff,
					   .pitch = image->pitch,
					   .width = image->width,
					   .height = image->height};

		aipl_color_convert(image->image, converted.image, image->pitch, image->width,
				   image->height, format, AIPL_COLOR_ARGB8888);

		dave2d_image_draw(d2_mode_argb8888, &converted);
	}
}

static void graph_image_destroy(graph_object_t *image)
{
	free(image->data);
}
