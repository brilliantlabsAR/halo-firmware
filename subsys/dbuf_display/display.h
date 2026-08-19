/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 *
 */

/*
 *   display.h
 */
#ifndef __DISPLAY_H
#define __DISPLAY_H

#include <stdint.h>
#include <zephyr/kernel.h>

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)

#define DISPLAY_WIDTH  DT_PROP(DISPLAY_NODE, width)
#define DISPLAY_HEIGHT DT_PROP(DISPLAY_NODE, height)

int display_init(void);
void display_set_next_frame_duration(uint32_t duration);
void display_next_frame(void);
void *display_active_buffer(void);
void *display_inactive_buffer(void);
uint32_t display_width(void);
uint32_t display_height(void);

#endif /* __DISPLAY_ILI9806_H */
