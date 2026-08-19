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
 * @file image.h
 */

#ifndef IMAGE_H
#define IMAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "aipl_image.h"

void aipl_dave2d_prepare(void);

void aipl_dave2d_render(void);

void aipl_image_draw(uint32_t x, uint32_t y, const aipl_image_t *image);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*IMAGE_H*/
