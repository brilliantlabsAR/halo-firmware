/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys_clock.h>
#include <stdlib.h>
#include <alif_ble.h>
#include "gapi_isooshm.h"
#include "iso_datapath_htoc.h"
#include "presentation_compensation.h"

LOG_MODULE_REGISTER(iso_datapath_htoc, CONFIG_BLE_AUDIO_LOG_LEVEL);

#if CONFIG_ALIF_BLE_AUDIO_USE_RAMFUNC
#define INT_RAMFUNC __ramfunc
#else
#define INT_RAMFUNC
#endif

#define GPIO_TEST0_NODE DT_ALIAS(htoc_test0)
#define GPIO_TEST1_NODE DT_ALIAS(htoc_test1)

#if DT_NODE_EXISTS(GPIO_TEST0_NODE) || DT_NODE_EXISTS(GPIO_TEST1_NODE)
static const struct gpio_dt_spec test_pin0 = GPIO_DT_SPEC_GET_OR(GPIO_TEST0_NODE, gpios, {0});
static const struct gpio_dt_spec test_pin1 = GPIO_DT_SPEC_GET_OR(GPIO_TEST1_NODE, gpios, {0});

static int init_test_pin(const struct gpio_dt_spec *const p_pin)
{
	if (!p_pin->port) {
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(p_pin)) {
		LOG_WRN("Test pin is not ready");
		return -ENODEV;
	}
	if (gpio_pin_configure_dt(p_pin, GPIO_OUTPUT_ACTIVE)) {
		LOG_ERR("Failed to configure test pin");
		return -EIO;
	}
	gpio_pin_set_dt(p_pin, 0);
	LOG_INF("HTOC test pin %u initialized", p_pin->pin);
	return 0;
}

static inline void toggle_test_pin(const struct gpio_dt_spec *const p_pin)
{
	if (!p_pin->port) {
		return;
	}
	gpio_pin_toggle_dt(p_pin);
}

static inline void set_test_pin(const struct gpio_dt_spec *const p_pin, int const val)
{
	if (!p_pin->port) {
		return;
	}
	gpio_pin_set_dt(p_pin, !!val);
}

/* Initialisation to perform pre-main */
static int iso_datapath_htoc_preinit(void)
{
	init_test_pin(&test_pin0);
	init_test_pin(&test_pin1);
	return 0;
}
SYS_INIT(iso_datapath_htoc_preinit, APPLICATION, 0);
#endif

struct iso_datapath_htoc {
	uint32_t stream_id;
	gapi_isooshm_dp_t dp;
	struct sdu_queue *sdu_queue;
	struct k_msgq sdu_timing_msgq;
	uint8_t *sdu_timing_msgq_buffer;
	uint16_t last_sdu_seq;
	bool timing_master_channel;
	bool awaiting_sdu;
};

struct sdu_timing_info {
	uint32_t capture_timestamp;
	uint16_t seq_num;
};

INT_RAMFUNC static void send_next_sdu(struct iso_datapath_htoc *const datapath, bool const lock)
{
	void *p_sdu = NULL;
	int ret = k_msgq_get(&datapath->sdu_queue->msgq, (void *)&p_sdu, K_NO_WAIT);

	if (ret || !p_sdu) {
		datapath->awaiting_sdu = true;
		return;
	}

	if (lock) {
		/* Lock is needed to protect in case of bidirectional transfer
		 * while DMA is not supported yet. Copy is synchronous at the moment
		 * which could cause a race condition if incoming data is processed.
		 * Can be removed when DMA is supported.
		 */
		alif_ble_mutex_lock(K_FOREVER);
	}
	ret = gapi_isooshm_dp_set_buf(&datapath->dp, p_sdu);
	if (lock) {
		alif_ble_mutex_unlock();
	}

	if (!ret) {
		return;
	}

	/* Free current current block (just ignore) and wait next trigger for retry */
	k_mem_slab_free(&datapath->sdu_queue->slab, p_sdu);
	datapath->awaiting_sdu = true;

	LOG_ERR("Failed to set next ISO buffer, err %u", ret);
}

INT_RAMFUNC static void on_dp_transfer_complete(gapi_isooshm_dp_t *const dp,
						gapi_isooshm_sdu_buf_t *const buf)
{
#if DT_NODE_EXISTS(GPIO_TEST0_NODE)
	set_test_pin(&test_pin0, 1);
#endif

	struct iso_datapath_htoc *const datapath = CONTAINER_OF(dp, struct iso_datapath_htoc, dp);

	send_next_sdu(datapath, false);

	if (buf) {
		k_mem_slab_free(&datapath->sdu_queue->slab, buf);
	}

#if DT_NODE_EXISTS(GPIO_TEST0_NODE)
	set_test_pin(&test_pin0, 0);
#endif
}

struct iso_datapath_htoc *iso_datapath_htoc_create(uint8_t const stream_lid,
						   struct sdu_queue *sdu_queue,
						   bool const timing_master_channel)
{

	struct iso_datapath_htoc *datapath =
		iso_datapath_htoc_init(stream_lid, sdu_queue, timing_master_channel);

	if (!datapath) {
		return NULL;
	}

	if (iso_datapath_htoc_bind(datapath)) {
		iso_datapath_htoc_delete(datapath);
		return NULL;
	}

	return datapath;
}

struct iso_datapath_htoc *iso_datapath_htoc_init(uint8_t const stream_lid,
						 struct sdu_queue *const sdu_queue,
						 bool const timing_master_channel)
{
	if (!sdu_queue) {
		LOG_ERR("Invalid parameter");
		return NULL;
	}

	struct iso_datapath_htoc *datapath = calloc(1, sizeof(*datapath));

	if (!datapath) {
		LOG_ERR("Failed to allocate data path");
		return NULL;
	}

	datapath->stream_id = stream_lid;
	datapath->sdu_queue = sdu_queue;
	datapath->timing_master_channel = timing_master_channel;

	if (timing_master_channel) {
		/* Timing queue should be slightly larger than SDU queue, as SDUs will be held for a
		 * short while on the controller before being sent
		 */
		size_t sdu_timing_queue_size = sdu_queue->item_count + 2;

		datapath->sdu_timing_msgq_buffer =
			(uint8_t *)malloc(sizeof(struct sdu_timing_info) * sdu_timing_queue_size);
		if (datapath->sdu_timing_msgq_buffer == NULL) {
			LOG_ERR("Failed to allocate timing queue");
			iso_datapath_htoc_delete(datapath);
			return NULL;
		}

		k_msgq_init(&datapath->sdu_timing_msgq, datapath->sdu_timing_msgq_buffer,
			    sizeof(struct sdu_timing_info), sdu_timing_queue_size);
		datapath->last_sdu_seq = UINT16_MAX;
	}

	uint16_t const ret = gapi_isooshm_dp_init(&datapath->dp, on_dp_transfer_complete);

	if (ret != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to init datapath with err %u", ret);
		iso_datapath_htoc_delete(datapath);
		return NULL;
	}

	return datapath;
}

int iso_datapath_htoc_bind(struct iso_datapath_htoc *const datapath)
{
	if (!datapath) {
		return -EINVAL;
	}

	uint16_t const ret =
		gapi_isooshm_dp_bind(&datapath->dp, datapath->stream_id, GAPI_DP_DIRECTION_INPUT);

	if (ret != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to bind datapath with err %u", ret);
		return -ENOEXEC;
	}

	/* Flag that datapath is waiting for first SDU */
	datapath->awaiting_sdu = true;

#if DT_NODE_EXISTS(GPIO_TEST0_NODE)
	set_test_pin(&test_pin0, 0);
#endif
#if DT_NODE_EXISTS(GPIO_TEST1_NODE)
	set_test_pin(&test_pin1, 0);
#endif

	return 0;
}

int iso_datapath_htoc_unbind(struct iso_datapath_htoc *const datapath)
{
	if (!datapath) {
		return -EINVAL;
	}

	datapath->awaiting_sdu = false;

	gapi_isooshm_sdu_buf_t *pending_buffer = NULL;

	gapi_isooshm_dp_unbind(&datapath->dp, &pending_buffer);

	if (pending_buffer) {
		/* Free the buffer that was pending in the datapath */
		k_mem_slab_free(&datapath->sdu_queue->slab, pending_buffer);
	}

	return 0;
}

INT_RAMFUNC static void store_sdu_timing_info(struct iso_datapath_htoc *iso_dp,
					      uint32_t capture_timestamp, uint16_t sdu_seq)
{
	struct sdu_timing_info info = {
		.seq_num = sdu_seq,
		.capture_timestamp = capture_timestamp,
	};

	int const ret = k_msgq_put(&iso_dp->sdu_timing_msgq, &info, K_NO_WAIT);

	if (ret) {
		LOG_ERR("Failed to put SDU timing to msgq, err %d", ret);
	}
}

INT_RAMFUNC static int get_sdu_timing(struct iso_datapath_htoc *iso_dp, uint16_t sdu_seq,
				      struct sdu_timing_info *info)
{
	/* Loop through SDU queue until we either find a matching SDU or we know that the matching
	 * SDU does not exist in the queue
	 */
	while (1) {
		int ret = k_msgq_peek(&iso_dp->sdu_timing_msgq, info);

		if (ret) {
			/* No messages remaining in msgq. Timing info cannot be found */
			LOG_WRN("SDU timing info cannot be found (no messages)");
			return -ENOMSG;
		}

		if (info->seq_num == sdu_seq) {
			/* Matching SDU timing info is found. Pop from queue and return */
			k_msgq_get(&iso_dp->sdu_timing_msgq, info, K_NO_WAIT);
			return 0;
		}

		if (((int32_t)sdu_seq - info->seq_num) > 0) {
			/* Timing info matches a previous SDU. Pop from queue and move on to next */
			k_msgq_get(&iso_dp->sdu_timing_msgq, info, K_NO_WAIT);
		} else {
			/* Timing info matches a future SDU, stop traversing SDU timing queue */
			LOG_WRN("SDU timing info cannot be found");
			return -ENOMSG;
		}
	}
}

INT_RAMFUNC void iso_datapath_htoc_notify_sdu_available(void *const datapath,
							uint32_t const capture_timestamp,
							uint16_t const sdu_seq)
{
	if (datapath == NULL) {
		LOG_ERR("null datapath");
		return;
	}

#if DT_NODE_EXISTS(GPIO_TEST1_NODE)
	toggle_test_pin(&test_pin1);
#endif

	struct iso_datapath_htoc *const iso_dp = datapath;

	if (iso_dp->awaiting_sdu) {
		iso_dp->awaiting_sdu = false;
		send_next_sdu(iso_dp, true);
	}

	if (!iso_dp->timing_master_channel) {
		/* Timing is controlled by another channel, so no action to take on SDU timing info
		 */
		return;
	}

	/* Store timing info of the SDU that was just enqueued */
	store_sdu_timing_info(iso_dp, capture_timestamp, sdu_seq);

	/* Get timing info of the last SDU that was sent by controller */
	gapi_isooshm_sdu_sync_t sync_info;

	if (gapi_isooshm_dp_get_sync(&iso_dp->dp, &sync_info)) {
		/* No SDU has been processed by the controller yet */
		return;
	}

	if (sync_info.seq_num == iso_dp->last_sdu_seq) {
		/* Timing info has already been processed for this SDU */
		return;
	}

	iso_dp->last_sdu_seq = sync_info.seq_num;

	struct sdu_timing_info capture_info;

	if (get_sdu_timing(iso_dp, sync_info.seq_num, &capture_info)) {
		/* Timing info not found for this SDU */
		return;
	}

	/* LOG_INF("Successful sdu"); */
	/* uint32_t presentation_delay = sync_info.sdu_anchor - capture_info.capture_timestamp; */

	/* presentation_compensation_notify_timing(presentation_delay); */
}

int iso_datapath_htoc_delete(struct iso_datapath_htoc *datapath)
{
	if (!datapath) {
		return -EINVAL;
	}

	iso_datapath_htoc_unbind(datapath);
	free(datapath->sdu_timing_msgq_buffer);
	free(datapath);

	return 0;
}
