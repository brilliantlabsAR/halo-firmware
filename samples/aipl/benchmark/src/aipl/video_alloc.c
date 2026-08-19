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
 * @file video_alloc.c
 *
 */

#include <aipl_video_alloc.h>
#include <dave_d0lib.h>

void *aipl_video_alloc(uint32_t size)
{
	return d0_allocvidmem(size);
}

void aipl_video_free(void *ptr)
{
	d0_freevidmem(ptr);
}
