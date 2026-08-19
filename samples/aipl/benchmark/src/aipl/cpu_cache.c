/* Copyright (C) 2023-2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 *
 */

#include "aipl_cache.h"
#include <zephyr/cache.h>

void aipl_cpu_cache_clean(const void *ptr, uint32_t size)
{
	sys_cache_data_flush_range((void *)ptr, size);
}

void aipl_cpu_cache_invalidate(const void *ptr, uint32_t size)
{
	sys_cache_data_invd_range((void *)ptr, size);
}
