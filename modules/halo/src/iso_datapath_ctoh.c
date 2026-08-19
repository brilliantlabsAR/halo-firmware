/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys_clock.h>
#include <stdlib.h>
#include <alif_ble.h>
#include "gapi_isooshm.h"
#include "iso_datapath_ctoh.h"
#include "halo/mem_manager.h"

LOG_MODULE_REGISTER(iso_datapath_ctoh, CONFIG_HALO_LOG_LEVEL);

#if CONFIG_ALIF_BLE_AUDIO_USE_RAMFUNC
#define INT_RAMFUNC __ramfunc
#else
#define INT_RAMFUNC
#endif

#define ISOSHM_INVALID_STATUS (GAPI_ISOOSHM_SDU_STATUS_LOST + 1)

struct iso_datapath_ctoh {
	gapi_isooshm_dp_t dp;
	struct sdu_queue *sdu_queue;
	uint32_t start_timestamp_us;
	uint8_t stream_id;
	bool stop;
	/* Set (from the transfer-complete ISR) when no SDU buffer was available to
	 * re-arm the datapath; test-and-cleared by the decoder thread when it frees
	 * one. atomic_t so the ISR set and the thread's consume don't race. */
	atomic_t awaiting_buffer;
};

INT_RAMFUNC static void finish_last_sdu(struct sdu_queue *sdu_queue,
					gapi_isooshm_sdu_buf_t *const p_sdu, size_t const stream_id,
					uint32_t const timestamp)
{

	if (p_sdu->status != GAPI_ISOOSHM_SDU_STATUS_VALID) {
		/* LOG_ERR("Invalid status %u", p_sdu->status); */
		k_mem_slab_free(&sdu_queue->slab, p_sdu);
		return;
	}

	if (p_sdu->timestamp < timestamp) {
		LOG_ERR("Invalid timestamp %u", p_sdu->timestamp);
	}

	if (k_msgq_put(&sdu_queue->msgq, (void *)&p_sdu, K_NO_WAIT)) {
		/* Failed to send for decoding -> just ignore the packet to avoid memory lost */
		k_mem_slab_free(&sdu_queue->slab, p_sdu);
		return;
	}

	/* Wake the consumer thread if it opted into signalling instead of polling.
	 * k_sem_give is ISR-safe. */
	if (sdu_queue->data_ready) {
		k_sem_give(sdu_queue->data_ready);
	}
}

INT_RAMFUNC static int recv_next_sdu(struct iso_datapath_ctoh *const datapath, bool const lock)
{
	gapi_isooshm_sdu_buf_t *p_sdu = NULL;
	struct sdu_queue *const sdu_queue = datapath->sdu_queue;
	/* Allocate a new SDU buffer */
	int ret = k_mem_slab_alloc(&sdu_queue->slab, (void **)&p_sdu, K_NO_WAIT);

	if (ret || !p_sdu) {
		atomic_set(&datapath->awaiting_buffer, 1);
		return -ENOMEM;
	}

	/* Set max size of SDU */
	p_sdu->sdu_len = sdu_queue->payload_size;
	/* Init status to invalid to make sure it is filled correctly */
	p_sdu->status = ISOSHM_INVALID_STATUS;
	p_sdu->seq_num = 0;
	p_sdu->timestamp = 0;

	if (lock) {
		/* Lock is needed to protect in case of bidirectional transfer
		 * while DMA is not supported yet. Copy is synchronous at the moment
		 * which could cause a race condition if incoming data is processed.
		 * Can be removed when DMA is supported.
		 */
		alif_ble_mutex_lock(K_FOREVER);
	}
	uint16_t const err = gapi_isooshm_dp_set_buf(&datapath->dp, p_sdu);
	if (lock) {
		alif_ble_mutex_unlock();
	}

	if (err) {
		LOG_ERR("Failed to set next ISO buffer, err %u", err);
		atomic_set(&datapath->awaiting_buffer, 1);
		k_mem_slab_free(&sdu_queue->slab, p_sdu);
		p_sdu = NULL;
		ret = -EIO;
	}

	return ret;
}

INT_RAMFUNC static void on_dp_transfer_complete(gapi_isooshm_dp_t *const dp,
						gapi_isooshm_sdu_buf_t *const buf)
{

	struct iso_datapath_ctoh *const datapath = CONTAINER_OF(dp, struct iso_datapath_ctoh, dp);
	if (!datapath->stop) {
		recv_next_sdu(datapath, false);
	}

	if (buf) {
		finish_last_sdu(datapath->sdu_queue, buf, datapath->stream_id,
				datapath->start_timestamp_us);
	}
}

static int iso_datapath_ctoh_bind(struct iso_datapath_ctoh *const datapath)
{
	uint16_t ret;

	ret = gapi_isooshm_dp_bind(&datapath->dp, datapath->stream_id, GAPI_DP_DIRECTION_OUTPUT);
	if (ret != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to bind datapath (stream_lid %u) with err %u", datapath->stream_id,
			ret);
		return -ENOEXEC;
	}

	return 0;
}

static int iso_datapath_ctoh_unbind(struct iso_datapath_ctoh *const datapath)
{

	gapi_isooshm_sdu_buf_t *pending_buffer = NULL;

	/* Ignore return value to allow application to call this even the
	 * datapath is not binded yet
	 */
	gapi_isooshm_dp_unbind(&datapath->dp, &pending_buffer);

	if (pending_buffer) {
		/* Free the buffer that was pending in the datapath */
		k_mem_slab_free(&datapath->sdu_queue->slab, pending_buffer);
	}

	return 0;
}

struct iso_datapath_ctoh *iso_datapath_ctoh_create(uint8_t const stream_lid,
						   struct sdu_queue *sdu_queue)
{
	struct iso_datapath_ctoh *datapath = iso_datapath_ctoh_init(stream_lid, sdu_queue);

	if (!datapath) {
		LOG_ERR("Failed to allocate data path");
		return NULL;
	}

	int ret = iso_datapath_ctoh_bind(datapath);

	if (ret) {
		LOG_ERR("Failed to bind datapath (stream_lid %u) with err %u", stream_lid, ret);
		iso_datapath_ctoh_delete(datapath);
		return NULL;
	}

	return datapath;
}

struct iso_datapath_ctoh *iso_datapath_ctoh_init(uint8_t const stream_lid,
						 struct sdu_queue *const sdu_queue)
{
	if (!sdu_queue) {
		LOG_ERR("Invalid parameter");
		return NULL;
	}

	struct iso_datapath_ctoh *const datapath = halo_mem_calloc(1, sizeof(*datapath));

	if (!datapath) {
		LOG_ERR("Failed to allocate data path");
		return NULL;
	}

	datapath->sdu_queue = sdu_queue;
	datapath->stream_id = stream_lid;

	uint16_t ret = gapi_isooshm_dp_init(&datapath->dp, on_dp_transfer_complete);

	if (ret != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to init datapath with err %u", ret);
		halo_free(datapath);
		return NULL;
	}

	return datapath;
}

int iso_datapath_ctoh_start(struct iso_datapath_ctoh *const datapath)
{
	if (!datapath) {
		return -EINVAL;
	}

	datapath->stop = false;
	atomic_set(&datapath->awaiting_buffer, 0);

	if (iso_datapath_ctoh_bind(datapath)) {
		LOG_ERR("Failed to bind datapath (stream_lid %u)", datapath->stream_id);
		return -ENOEXEC;
	}

	int err = recv_next_sdu(datapath, true);

	/* TODO: handle start timestamp
	 * datapath->start_timestamp_us = gapi_isooshm_dp_get_local_time();
	 */

	return err;
}

int iso_datapath_ctoh_stop(struct iso_datapath_ctoh *const datapath)
{
	if (!datapath) {
		return -EINVAL;
	}

	datapath->stop = true;
	atomic_set(&datapath->awaiting_buffer, 0);

	return iso_datapath_ctoh_unbind(datapath);
}

INT_RAMFUNC void iso_datapath_ctoh_notify_sdu_done(void *p_datapath, uint32_t const timestamp,
						   uint16_t const sdu_seq)
{
	ARG_UNUSED(timestamp);
	ARG_UNUSED(sdu_seq);

	struct iso_datapath_ctoh *const datapath = p_datapath;

	if (!datapath || datapath->stop) {
		return;
	}

	/* Consume the pending re-arm request atomically: only the context that
	 * flips it 1->0 proceeds, so an ISR set and this consume can't both fire
	 * recv_next_sdu for the same starvation. */
	if (!atomic_cas(&datapath->awaiting_buffer, 1, 0)) {
		return;
	}

	recv_next_sdu(datapath, true);
}

int iso_datapath_ctoh_delete(struct iso_datapath_ctoh *const datapath)
{
	if (!datapath) {
		return -EINVAL;
	}

	iso_datapath_ctoh_unbind(datapath);
	halo_free(datapath);

	return 0;
}
