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
 * @file assets.h
 */

#ifndef ASSETS_H
#define ASSETS_H

#include <stdint.h>

enum color_format_t {
	COLOR_ALPHA8,
	COLOR_ARGB8888,
	COLOR_ARGB4444,
	COLOR_ARGB1555,
	COLOR_RGBA8888,
	COLOR_RGBA4444,
	COLOR_RGBA5551,
	COLOR_BGR888,
	COLOR_RGB888,
	COLOR_RGB565,
	COLOR_YV12,
	COLOR_I420,
	COLOR_I422,
	COLOR_I444,
	COLOR_I400,
	COLOR_NV21,
	COLOR_NV12,
	COLOR_YUY2,
	COLOR_UYVY,
};

typedef struct {
	void *data;
	uint32_t data_size;
	uint32_t pitch;
	uint32_t width;
	uint32_t height;
	uint32_t format;
} image_t;

#define ASSET_PREFIX

#ifndef ASSET_INTERNAL
extern const image_t SAMPLE_PHOTO_RGB888;
extern const image_t SAMPLE_PHOTO_ARGB8888;
extern const image_t SAMPLE_PHOTO_ARGB4444;
extern const image_t SAMPLE_PHOTO_ARGB1555;
extern const image_t SAMPLE_PHOTO_RGBA8888;
extern const image_t SAMPLE_PHOTO_RGBA4444;
extern const image_t SAMPLE_PHOTO_RGBA5551;
extern const image_t SAMPLE_PHOTO_RGB565;
extern const image_t SAMPLE_PHOTO_ALPHA8;
extern const image_t SAMPLE_PHOTO_ALPHA4;
extern const image_t SAMPLE_PHOTO_ALPHA2;
extern const image_t SAMPLE_PHOTO_ALPHA1;
#endif

#endif /* ASSETS_H */
