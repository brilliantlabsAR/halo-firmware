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
 * @file graphics.c
 */

#include "graphics.h"
#include "dbuf_display/display.h"
#include "aipl_dave2d.h"
#include <stdlib.h>
#include <string.h>

graph_frame_t graph_create_frame(uint32_t duration)
{
	graph_frame_t frame = {NULL, duration, NULL, d2_mode_argb8888};

	return frame;
}

graph_scene_t graph_create_scene(void)
{
	graph_scene_t scene = {NULL};

	return scene;
}

void graph_draw_object(const graph_object_t *object)
{
	d2_device *handle = aipl_dave2d_handle();

	d2_endframe(handle);

	object->draw_func(object->color_format, object->data);

	d2_startframe(handle);
}

void graph_draw_frame(const graph_frame_t *frame)
{
	d2_device *handle = aipl_dave2d_handle();

	d2_framebuffer(handle, display_inactive_buffer(), display_width(), display_height(),
		       display_height(), d2_mode_rgb565);

	if (frame->clut != NULL) {
		/* dave2d_set_clut((d2_color*)frame->clut, frame->clut_format); */
	}

	d2_endframe(handle);
	d2_startframe(handle);

	d2_clear(handle, 0x00f0f0f0);

	graph_object_ll_t *node = frame->objects;

	while (node != NULL) {
		graph_draw_object(&node->object);

		node = node->next;
	}

	d2_endframe(handle);

	display_next_frame();

	display_set_next_frame_duration(frame->duration);
}

void graph_draw_scene(const graph_scene_t *scene)
{
	graph_frame_ll_t *node = scene->frames;

	while (node != NULL) {
		graph_draw_frame(&node->frame);

		node = node->next;
	}
}

void graph_frame_add_object(graph_frame_t *frame, const graph_object_t *object)
{
	graph_object_ll_t **node = &frame->objects;

	while (*node != NULL) {
		node = &(*node)->next;
	}

	*node = (graph_object_ll_t *)malloc(sizeof(graph_object_ll_t));
	memcpy(&(*node)->object, object, sizeof(graph_object_t));
	(*node)->next = NULL;
}

void graph_frame_set_clut(graph_frame_t *frame, uint32_t *clut, uint32_t format)
{
	frame->clut = clut;
	frame->clut_format = format;
}

void graph_scene_add_frame(graph_scene_t *scene, const graph_frame_t *frame)
{
	graph_frame_ll_t **node = &scene->frames;

	while (*node != NULL) {
		node = &(*node)->next;
	}

	*node = (graph_frame_ll_t *)malloc(sizeof(graph_frame_ll_t));
	memcpy(&(*node)->frame, frame, sizeof(graph_frame_t));
	(*node)->next = NULL;
}

void graph_destroy_object(graph_object_t *object)
{
	object->destroy_func(object);
}

void graph_destroy_frame(graph_frame_t *frame)
{
	graph_object_ll_t *node = frame->objects;

	while (node != NULL) {
		graph_object_ll_t *tmp = node;

		node = node->next;

		graph_destroy_object(&tmp->object);

		free(tmp);
	}
}

void graph_destroy_scene(graph_scene_t *scene)
{
	graph_frame_ll_t *node = scene->frames;

	while (node != NULL) {
		graph_frame_ll_t *tmp = node;

		node = node->next;

		graph_destroy_frame(&tmp->frame);

		free(tmp);
	}
}

void graph_clear_screen(void)
{
	d2_device *handle = aipl_dave2d_handle();

	d2_clear(handle, 0x00f0f0f0);
}
