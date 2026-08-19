/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef _SDU_QUEUE_H
#define _SDU_QUEUE_H

#include <zephyr/kernel.h>

struct sdu_queue {
	size_t item_count;
	size_t item_size;
	size_t payload_size;
	struct k_mem_slab slab;
	struct k_msgq msgq;
	uint8_t buf[];
};

/**
 * @brief Create and initialise an SDU queue
 *
 * In general the size of each SDU is not known until the parameters of the stream have been agreed
 * between this device and the peer device (the broadcast source use-case is an exception, where the
 * parameters can be known at compile time). This function dynamically allocates and initialises an
 * instance of an SDU queue.
 *
 * @param item_count Number of SDUs in the queue
 * @param payload_size Size of each SDU payload, excluding the SDU header
 *
 * @retval Pointer to created SDU queue header if successful
 * @retval NULL if an error occurred
 */
struct sdu_queue *sdu_queue_create(size_t item_count, size_t payload_size);

/**
 * @brief Delete an SDU queue
 *
 * @param queue Pointer to the queue to delete
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int sdu_queue_delete(struct sdu_queue *queue);

#endif /* _SDU_QUEUE_H */
