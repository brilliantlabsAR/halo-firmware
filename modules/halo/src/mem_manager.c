/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_heap.h>
#include <string.h>
#include <stdint.h>
#include "halo/mem_manager.h"

LOG_MODULE_REGISTER(halo_mem, CONFIG_HALO_LOG_LEVEL);

/* Static internal memory pool */
static uint8_t internal_mem_pool[CONFIG_HALO_MEM_INTERNAL_SIZE] __aligned(8);

/* Memory manager context */
static struct {
	bool initialized;
	struct k_mutex lock;

	/* Internal heap */
	struct sys_heap internal_heap;
	size_t internal_size;

#if defined(CONFIG_HALO_MEM_USE_EXTERNAL_SRAM)
	/* External SRAM heap */
	struct sys_heap external_heap;
	uint8_t *external_mem;
	size_t external_size;
#endif
} mem_ctx;

int halo_mem_init(void)
{
	if (mem_ctx.initialized) {
		return 0;
	}

	memset(&mem_ctx, 0, sizeof(mem_ctx));
	k_mutex_init(&mem_ctx.lock);

	/* Initialize internal heap from static pool */
	mem_ctx.internal_size = CONFIG_HALO_MEM_INTERNAL_SIZE;

	sys_heap_init(&mem_ctx.internal_heap, internal_mem_pool, mem_ctx.internal_size);

	LOG_DBG("Internal heap initialized at %08x, size %u bytes",
		(uint32_t)internal_mem_pool, mem_ctx.internal_size);

#if defined(CONFIG_HALO_MEM_USE_EXTERNAL_SRAM)
	/* Initialize external SRAM heap */
	uintptr_t base_addr = (uintptr_t)CONFIG_HALO_MEM_EXTERNAL_SRAM_ADDR;
	uintptr_t offset = (uintptr_t)CONFIG_HALO_MEM_EXTERNAL_SRAM_OFFSET;
	
	mem_ctx.external_mem = (uint8_t *)(base_addr + offset);
	mem_ctx.external_size = CONFIG_HALO_MEM_EXTERNAL_SRAM_SIZE;

	/* Check if external memory configuration is valid */
	if (mem_ctx.external_size == 0) {
		LOG_WRN("External SRAM size is 0, external memory disabled");
	} else if (mem_ctx.external_mem == NULL) {
		LOG_WRN("External SRAM address is NULL (base=0x%08x, offset=%lu), external memory disabled",
			(uint32_t)base_addr, offset);
	} else {
		sys_heap_init(&mem_ctx.external_heap, mem_ctx.external_mem, mem_ctx.external_size);

		LOG_DBG("External SRAM heap initialized at 0x%08x (base 0x%08x + offset %lu), size %u bytes",
			(uint32_t)mem_ctx.external_mem, (uint32_t)base_addr, offset,
			mem_ctx.external_size);
	}
#endif

	mem_ctx.initialized = true;

	return 0;
}

void *halo_malloc(size_t size, halo_mem_region_t region)
{
	void *ptr = NULL;

	if (!mem_ctx.initialized) {
		LOG_ERR("Memory manager not initialized");
		return NULL;
	}

	if (size == 0) {
		return NULL;
	}

	/* Choose allocation strategy */
	switch (region) {
	case HALO_MEM_REGION_INTERNAL:
		/* Use internal heap */
		ptr = sys_heap_alloc(&mem_ctx.internal_heap, size);
		break;

#if defined(CONFIG_HALO_MEM_USE_EXTERNAL_SRAM)
	case HALO_MEM_REGION_EXTERNAL:
		/* Use external SRAM heap */
		if (mem_ctx.external_mem != NULL && mem_ctx.external_size > 0) {
			ptr = sys_heap_alloc(&mem_ctx.external_heap, size);
		} else {
			LOG_WRN("External SRAM not available, allocation failed");
			ptr = NULL;
		}
		break;
#endif

	case HALO_MEM_REGION_AUTO:
	default:
		/* Try internal first, then external if available */
		ptr = sys_heap_alloc(&mem_ctx.internal_heap, size);
#if defined(CONFIG_HALO_MEM_USE_EXTERNAL_SRAM)
		if (!ptr && mem_ctx.external_mem != NULL && mem_ctx.external_size > 0) {
			ptr = sys_heap_alloc(&mem_ctx.external_heap, size);
		}
#endif
		break;
	}

	if (!ptr) {
		LOG_ERR("Failed to allocate %u bytes from region %d", size, region);
		return NULL;
	}

	return ptr;
}

void *halo_calloc(size_t nmemb, size_t size, halo_mem_region_t region)
{
	/* Check for integer overflow */
	if (nmemb != 0 && size > SIZE_MAX / nmemb) {
		LOG_ERR("Integer overflow in calloc: %zu * %zu", nmemb, size);
		return NULL;
	}
	
	size_t total_size = nmemb * size;
	void *ptr = halo_malloc(total_size, region);

	if (ptr) {
		memset(ptr, 0, total_size);
	}

	return ptr;
}

void *halo_realloc(void *ptr, size_t size, halo_mem_region_t region)
{
	if (!ptr) {
		return halo_malloc(size, region);
	}

	if (size == 0) {
		halo_free(ptr);
		return NULL;
	}

	/* Determine which heap the old pointer belongs to and get its size */
	struct sys_heap *old_heap = NULL;
	size_t old_size = 0;

	if ((uint8_t *)ptr >= internal_mem_pool &&
	    (uint8_t *)ptr < internal_mem_pool + CONFIG_HALO_MEM_INTERNAL_SIZE) {
		old_heap = &mem_ctx.internal_heap;
	}
#if defined(CONFIG_HALO_MEM_USE_EXTERNAL_SRAM)
	else if ((uint8_t *)ptr >= mem_ctx.external_mem &&
		 (uint8_t *)ptr < mem_ctx.external_mem + mem_ctx.external_size) {
		old_heap = &mem_ctx.external_heap;
	}
#endif

	if (old_heap) {
		old_size = sys_heap_usable_size(old_heap, ptr);
	}

	if (old_size == 0) {
		LOG_ERR("Invalid pointer or cannot determine size");
		return NULL;
	}

	/* Allocate new block */
	void *new_ptr = halo_malloc(size, region);
	if (!new_ptr) {
		/* On allocation failure, preserve original pointer (standard realloc behavior) */
		LOG_ERR("Realloc failed to allocate %zu bytes, original pointer preserved", size);
		return NULL;
	}

	/* Copy data from old block to new block - copy the minimum of old and new sizes */
	size_t copy_size = (old_size < size) ? old_size : size;
	memcpy(new_ptr, ptr, copy_size);

	/* Free old block */
	halo_free(ptr);

	return new_ptr;
}

void halo_free(void *ptr)
{
	if (!ptr) {
		return;
	}

	if (!mem_ctx.initialized) {
		LOG_ERR("Memory manager not initialized");
		return;
	}

	/* Determine which heap the pointer belongs to */
	if ((uint8_t *)ptr >= internal_mem_pool &&
	    (uint8_t *)ptr < internal_mem_pool + CONFIG_HALO_MEM_INTERNAL_SIZE) {
		sys_heap_free(&mem_ctx.internal_heap, ptr);
	}
#if defined(CONFIG_HALO_MEM_USE_EXTERNAL_SRAM)
	else if ((uint8_t *)ptr >= mem_ctx.external_mem &&
		 (uint8_t *)ptr < mem_ctx.external_mem + mem_ctx.external_size) {
		sys_heap_free(&mem_ctx.external_heap, ptr);
	}
#endif
	else {
		LOG_ERR("Invalid pointer %p, not from managed heaps", ptr);
		return;
	}
}

int halo_mem_get_stats(halo_mem_region_t region, halo_mem_stats_t *stats)
{
	if (!stats) {
		return -EINVAL;
	}

	if (!mem_ctx.initialized) {
		return -ENODEV;
	}

	memset(stats, 0, sizeof(halo_mem_stats_t));

#if defined(CONFIG_HALO_MEM_MANAGER_STATS)
	switch (region) {
	case HALO_MEM_REGION_INTERNAL: {
		struct sys_memory_stats heap_stats;
		sys_heap_runtime_stats_get(&mem_ctx.internal_heap, &heap_stats);

		stats->total_size = mem_ctx.internal_size;
		stats->used_size = heap_stats.allocated_bytes;
		stats->free_size = heap_stats.free_bytes;
		stats->max_block_size = heap_stats.max_allocated_bytes;
		break;
	}

#if defined(CONFIG_HALO_MEM_USE_EXTERNAL_SRAM)
	case HALO_MEM_REGION_EXTERNAL: {
		struct sys_memory_stats heap_stats;
		sys_heap_runtime_stats_get(&mem_ctx.external_heap, &heap_stats);

		stats->total_size = mem_ctx.external_size;
		stats->used_size = heap_stats.allocated_bytes;
		stats->free_size = heap_stats.free_bytes;
		stats->max_block_size = heap_stats.max_allocated_bytes;
		break;
	}
#endif

	case HALO_MEM_REGION_AUTO:
	default: {
		/* Combined stats from both regions */
		struct sys_memory_stats internal_stats;
		sys_heap_runtime_stats_get(&mem_ctx.internal_heap, &internal_stats);

		stats->total_size = mem_ctx.internal_size;
		stats->used_size = internal_stats.allocated_bytes;
		stats->free_size = internal_stats.free_bytes;
		stats->max_block_size = internal_stats.max_allocated_bytes;

#if defined(CONFIG_HALO_MEM_USE_EXTERNAL_SRAM)
		struct sys_memory_stats external_stats;
		sys_heap_runtime_stats_get(&mem_ctx.external_heap, &external_stats);

		stats->total_size += mem_ctx.external_size;
		stats->used_size += external_stats.allocated_bytes;
		stats->free_size += external_stats.free_bytes;
		if (external_stats.max_allocated_bytes > stats->max_block_size) {
			stats->max_block_size = external_stats.max_allocated_bytes;
		}
#endif
		break;
	}
	}
#else
	/* Statistics disabled - only report total size */
	switch (region) {
	case HALO_MEM_REGION_INTERNAL:
		stats->total_size = mem_ctx.internal_size;
		break;
#if defined(CONFIG_HALO_MEM_USE_EXTERNAL_SRAM)
	case HALO_MEM_REGION_EXTERNAL:
		stats->total_size = mem_ctx.external_size;
		break;
#endif
	case HALO_MEM_REGION_AUTO:
	default:
		stats->total_size = mem_ctx.internal_size;
#if defined(CONFIG_HALO_MEM_USE_EXTERNAL_SRAM)
		stats->total_size += mem_ctx.external_size;
#endif
		break;
	}
	LOG_WRN("Runtime statistics disabled (CONFIG_HALO_MEM_MANAGER_STATS=n)");
#endif

	return 0;
}
