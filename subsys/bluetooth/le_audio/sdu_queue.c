/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <stdlib.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/__assert.h>
#include "gapi_isooshm.h"
#include "sdu_queue.h"

LOG_MODULE_REGISTER(sdu_queue, CONFIG_BLE_AUDIO_LOG_LEVEL);

struct sdu_queue *sdu_queue_create(size_t item_count, size_t payload_size)
{
	size_t item_size = payload_size + sizeof(gapi_isooshm_sdu_buf_t);

	/* Each item must be 4-byte aligned */
	size_t padded_size = ROUND_UP(item_size, 4);

	size_t total_size = sizeof(struct sdu_queue) + (item_count * padded_size) +
			    (item_count * sizeof(void *));

	struct sdu_queue *hdr = (struct sdu_queue *)malloc(total_size);

	if (hdr == NULL) {
		LOG_ERR("Failed to allocate SDU queue");
		return NULL;
	}

	/* malloc should give a minimum of 4-byte alignment, but confirm this */
	if (!IS_PTR_ALIGNED(hdr->buf, 4)) {
		free(hdr);
		LOG_ERR("SDU buffer is not 4-byte aligned");
		return NULL;
	}

	int ret = k_mem_slab_init(&hdr->slab, hdr->buf, padded_size, item_count);

	if (ret) {
		free(hdr);
		LOG_ERR("Failed to initialise SDU queue mem slab");
		return NULL;
	}

	k_msgq_init(&hdr->msgq, hdr->buf + (item_count * padded_size), sizeof(void *), item_count);

	hdr->payload_size = payload_size;
	hdr->item_count = item_count;
	hdr->item_size = item_size;

	return hdr;
}

int sdu_queue_delete(struct sdu_queue *queue)
{
	if (queue == NULL) {
		return -EINVAL;
	}

	free(queue);
	return 0;
}
