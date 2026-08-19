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
 * @file graphics.h
 */

#ifndef GRAPHICS_H
#define GRAPHICS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef void (*graph_draw_func_t)(uint32_t format, void *data);

typedef struct graph_object graph_object_t;

typedef void (*graph_destroy_func_t)(graph_object_t *obj);

struct graph_object {
	graph_draw_func_t draw_func;
	uint32_t color_format;
	void *data;
	graph_destroy_func_t destroy_func;
};

typedef struct graph_object_ll graph_object_ll_t;
struct graph_object_ll {
	graph_object_t object;
	graph_object_ll_t *next;
};

typedef struct {
	graph_object_ll_t *objects;
	uint32_t duration;
	uint32_t *clut;
	uint32_t clut_format;
} graph_frame_t;

typedef struct graph_frame_ll graph_frame_ll_t;
struct graph_frame_ll {
	graph_frame_t frame;
	graph_frame_ll_t *next;
};

typedef struct {
	graph_frame_ll_t *frames;
} graph_scene_t;

graph_frame_t graph_create_frame(uint32_t duration);

graph_scene_t graph_create_scene(void);

void graph_draw_object(const graph_object_t *object);

void graph_draw_frame(const graph_frame_t *frame);

void graph_draw_scene(const graph_scene_t *scene);

void graph_frame_add_object(graph_frame_t *frame, const graph_object_t *object);

void graph_frame_set_clut(graph_frame_t *frame, uint32_t *clut, uint32_t format);

void graph_scene_add_frame(graph_scene_t *scene, const graph_frame_t *frame);

void graph_destroy_object(graph_object_t *object);

void graph_destroy_frame(graph_frame_t *frame);

void graph_destroy_scene(graph_scene_t *scene);

void graph_clear_screen(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*GRAPHICS_H*/
