/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_MEM_MANAGER_H_
#define HALO_MEM_MANAGER_H_

#include <zephyr/kernel.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Memory region types
 */
typedef enum {
	HALO_MEM_REGION_INTERNAL, /**< Internal memory  */
	HALO_MEM_REGION_EXTERNAL, /**< External SRAM  */
	HALO_MEM_REGION_AUTO,     /**< Auto-select best region */
} halo_mem_region_t;

/**
 * @brief Memory allocation statistics
 */
typedef struct {
	size_t total_size;      /**< Total memory size */
	size_t used_size;       /**< Currently used bytes */
	size_t free_size;       /**< Currently free bytes */
	size_t max_block_size;  /**< Largest allocatable block */
} halo_mem_stats_t;

/**
 * @brief Initialize memory manager
 *
 * @return 0 on success, negative errno otherwise
 */
int halo_mem_init(void);

/**
 * @brief Allocate memory
 *
 * @param size Size in bytes to allocate
 * @param region Memory region preference
 * @return Pointer to allocated memory, or NULL on failure
 */
void *halo_malloc(size_t size, halo_mem_region_t region);

/**
 * @brief Allocate zeroed memory
 *
 * @param nmemb Number of elements
 * @param size Size of each element
 * @param region Memory region preference
 * @return Pointer to allocated memory, or NULL on failure
 */
void *halo_calloc(size_t nmemb, size_t size, halo_mem_region_t region);

/**
 * @brief Reallocate memory
 *
 * @param ptr Pointer to existing allocation
 * @param size New size in bytes
 * @param region Memory region preference
 * @return Pointer to reallocated memory, or NULL on failure
 */
void *halo_realloc(void *ptr, size_t size, halo_mem_region_t region);

/**
 * @brief Free memory
 *
 * @param ptr Pointer to free (NULL-safe)
 */
void halo_free(void *ptr);

/**
 * @brief Get memory statistics
 *
 * @param region Memory region to query (or AUTO for combined)
 * @param stats Pointer to statistics structure to fill
 * @return 0 on success, negative errno otherwise
 */
int halo_mem_get_stats(halo_mem_region_t region, halo_mem_stats_t *stats);

/**
 * @brief Simple malloc wrapper (uses DTCM by default)
 *
 * Convenience macro for most common use case.
 */
#define halo_mem_alloc(size) halo_malloc(size, HALO_MEM_REGION_AUTO)

/**
 * @brief Simple calloc wrapper (uses DTCM by default)
 */
#define halo_mem_calloc(nmemb, size) halo_calloc(nmemb, size, HALO_MEM_REGION_AUTO)

#ifdef __cplusplus
}
#endif

#endif /* HALO_MEM_MANAGER_H_ */
