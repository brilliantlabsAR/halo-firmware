/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/check.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <stdlib.h>

#include "alif_lc3.h"
#include "gapi_isooshm.h"

#include "bluetooth/le_audio/audio_source_i2s.h"
#include "bluetooth/le_audio/iso_datapath_htoc.h"
#include "bluetooth/le_audio/audio_encoder.h"

#if CONFIG_ALIF_BLE_AUDIO_USE_RAMFUNC
#define INT_RAMFUNC __ramfunc
#else
#define INT_RAMFUNC
#endif

#define AUDIO_QUEUE_MARGIN_US     (CONFIG_ALIF_BLE_AUDIO_PRESENTATION_DELAY_QUEUE_MARGIN * 1000)
#define MIN_PRESENTATION_DELAY_US (CONFIG_ALIF_BLE_AUDIO_MIN_PRESENTATION_DELAY_MS * 1000)

LOG_MODULE_REGISTER(audio_encoder, CONFIG_BLE_AUDIO_LOG_LEVEL);

#define GPIO_TEST0_NODE DT_ALIAS(encoder_test0)
#define GPIO_TEST1_NODE DT_ALIAS(encoder_test1)

#if DT_NODE_EXISTS(GPIO_TEST0_NODE) || DT_NODE_EXISTS(GPIO_TEST1_NODE)
#if DT_NODE_EXISTS(GPIO_TEST0_NODE)
static const struct gpio_dt_spec test_pin0 = GPIO_DT_SPEC_GET_OR(GPIO_TEST0_NODE, gpios, {0});
#endif
#if DT_NODE_EXISTS(GPIO_TEST1_NODE)
static const struct gpio_dt_spec test_pin1 = GPIO_DT_SPEC_GET_OR(GPIO_TEST1_NODE, gpios, {0});
#endif

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
	LOG_INF("Encoder test pin %u initialized", p_pin->pin);
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
#endif

/* Initialisation to perform pre-main */
static int audio_encoder_init(void)
{
	int ret = alif_lc3_init();

	if (ret) {
		LOG_ERR("Failed to initialise LC3 codec");
		return ret;
	}

#if DT_NODE_EXISTS(GPIO_TEST0_NODE)
	init_test_pin(&test_pin0);
#endif
#if DT_NODE_EXISTS(GPIO_TEST1_NODE)
	init_test_pin(&test_pin1);
#endif
	return 0;
}
SYS_INIT(audio_encoder_init, APPLICATION, 0);

struct cb_list {
	audio_encoder_sdu_cb_t cb;
	void *context;
	struct cb_list *next;
};

struct channel_data {
	/* Input and output queues */
	struct sdu_queue *sdu_queue;
	struct iso_datapath_htoc *iso_dp;
	lc3_encoder_t *lc3_encoder;
	uint32_t stream_id;
	bool enabled;
};

struct audio_encoder {
	volatile bool thread_abort;
	struct audio_queue *audio_queue;
	struct channel_data channel[CONFIG_ALIF_BLE_AUDIO_NMB_CHANNELS];
	/* LC3 configuration, encoder instances and scratch memory */
	lc3_cfg_t lc3_cfg;
	int32_t *lc3_scratch;
	/* Linked list of registered callbacks */
	struct cb_list *cb_list;
	/* Encoder thread */
	struct k_thread thread;
	k_tid_t tid;
};

K_THREAD_STACK_DEFINE(encoder_stack, CONFIG_LC3_ENCODER_STACK_SIZE);

static int alloc_channel_index(struct audio_encoder const *const encoder)
{
	for (size_t iter = 0; iter < ARRAY_SIZE(encoder->channel); iter++) {
		if (encoder->channel[iter].stream_id == UINT32_MAX) {
			return iter;
		}
	}
	return -EINVAL;
}

static int get_channel_index(struct audio_encoder const *const encoder, uint32_t stream_id)
{
	for (size_t iter = 0; iter < ARRAY_SIZE(encoder->channel); iter++) {
		if (encoder->channel[iter].stream_id == stream_id) {
			return iter;
		}
	}

	return -EINVAL;
}

INT_RAMFUNC static void audio_encoder_thread_func(void *p1, void *p2, void *p3)
{
	struct audio_encoder *enc = (struct audio_encoder *)p1;
	(void)p2;
	(void)p3;

	LOG_DBG("Encoder thread started");

	int ret;
	size_t iter;
	gapi_isooshm_sdu_buf_t *p_sdu;
	struct sdu_queue *p_sdu_queue;
	struct k_mem_slab *const p_audio_queue_slab = &enc->audio_queue->slab;
	struct k_msgq *const p_audio_msg_queue = &enc->audio_queue->msgq;
	struct audio_block *audio;
	/* Sequence number applied to each outging SDU clipped to uint16_t */
	size_t sdu_seq = 0;

#if DT_NODE_EXISTS(GPIO_TEST0_NODE)
	set_test_pin(&test_pin0, 0);
#endif
#if DT_NODE_EXISTS(GPIO_TEST1_NODE)
	set_test_pin(&test_pin1, 0);
#endif

	while (!enc->thread_abort) {

		/* Get the next audio block */
		audio = NULL;
		ret = k_msgq_get(p_audio_msg_queue, &audio, K_FOREVER);
		if (ret) {
			k_sleep(K_MSEC(2));
			LOG_ERR("Failed to get audio block");
			continue;
		}

#if DT_NODE_EXISTS(GPIO_TEST1_NODE)
		set_test_pin(&test_pin1, 1);
#endif

		/* A NULL audio block can be sent to the queue to wake up and abort thread. Continue
		 * and check thread abort flag.
		 */
		if (!audio) {
			if (enc->thread_abort) {
				LOG_WRN("Thread aborted");
				break;
			}
			continue;
		}

		uint32_t const capture_timestamp = audio->timestamp;

		size_t const num_channels = MIN(ARRAY_SIZE(enc->channel), audio->num_channels);

		iter = num_channels;
		while (iter--) {
			struct channel_data *const p_channel = &enc->channel[iter];

			p_sdu = NULL;
			p_sdu_queue = p_channel->sdu_queue;

			if (!p_sdu_queue || !p_channel->enabled) {
				continue;
			}
			size_t const sdu_len = p_sdu_queue->payload_size;

			/* Allocate SDU and encode audio into it */
			ret = k_mem_slab_alloc(&p_sdu_queue->slab, (void *)&p_sdu, K_NO_WAIT);
			if (ret || !p_sdu) {
				LOG_WRN("SDU queue %u is full", iter);
				continue;
			}

#if DT_NODE_EXISTS(GPIO_TEST0_NODE)
			set_test_pin(&test_pin0, 1);
#endif
			ret = lc3_api_encode_frame(&enc->lc3_cfg, p_channel->lc3_encoder,
						   audio->channels[iter], p_sdu->data, sdu_len,
						   enc->lc3_scratch);
#if DT_NODE_EXISTS(GPIO_TEST0_NODE)
			set_test_pin(&test_pin0, 0);
#endif
			if (ret) {
				k_mem_slab_free(&p_sdu_queue->slab, p_sdu);
				LOG_ERR("LC3 encoding failed, err %d", ret);
				continue;
			}

			p_sdu->sdu_len = sdu_len;
			p_sdu->seq_num = sdu_seq;
			p_sdu->has_timestamp = !!capture_timestamp;
			p_sdu->timestamp = capture_timestamp;

			ret = k_msgq_put(&p_sdu_queue->msgq, &p_sdu, K_FOREVER);
			if (ret) {
				k_mem_slab_free(&p_sdu_queue->slab, p_sdu);
				LOG_ERR("Failed to put SDU to msgq, err %d", ret);
				continue;
			}
			/* Notify datapath that SDUs are completed. This also triggers next read
			 * if last one was failed for some reason.
			 */
			if (p_channel->iso_dp) {
				iso_datapath_htoc_notify_sdu_available(p_channel->iso_dp,
								       capture_timestamp, sdu_seq);
			}
		}

		k_mem_slab_free(p_audio_queue_slab, audio);

		/* Notify listeners that a block is completed */
		struct cb_list *cb_item = enc->cb_list;

		while (cb_item) {
			cb_item->cb(cb_item->context, capture_timestamp, sdu_seq);
			cb_item = cb_item->next;
		}

		/* Increment sequence number for next SDU */
		sdu_seq++;

#if DT_NODE_EXISTS(GPIO_TEST1_NODE)
		set_test_pin(&test_pin1, 0);
#endif
	}
}

struct audio_encoder *audio_encoder_create(struct audio_encoder_params const *params)
{
	int ret;
	struct audio_encoder *enc;

	if (!params) {
		LOG_ERR("Audio encoder parameters must be provided");
		return NULL;
	}

	if (ARRAY_SIZE(enc->channel) < params->num_queues) {
		LOG_ERR("Too many queues!");
		return NULL;
	}

	enc = calloc(1, sizeof(*enc));

	if (!enc) {
		LOG_ERR("Failed to allocate audio encoder");
		return NULL;
	}

	for (size_t iter = 0; iter < ARRAY_SIZE(enc->channel); iter++) {
		enc->channel[iter].stream_id = UINT32_MAX;
	}

	for (size_t iter = 0; iter < params->num_queues; iter++) {
		enc->channel[iter].sdu_queue = params->p_sdu_queues[iter];
	}

	/* Presentation delay less than a certain value is impossible due to latency of audio
	 * datapath
	 */
	uint32_t const buffer_len_us = MIN(params->audio_buffer_len_us, MIN_PRESENTATION_DELAY_US);
	size_t const audio_queue_len_blocks =
		1 + (buffer_len_us + AUDIO_QUEUE_MARGIN_US) / params->frame_duration_us;

	enc->audio_queue = audio_queue_create(audio_queue_len_blocks, params->sampling_rate_hz,
					      params->frame_duration_us);

	if (!enc->audio_queue) {
		free(enc);
		LOG_ERR("Failed to create audio queue");
		return NULL;
	}

	ret = audio_source_i2s_configure(params->i2s_dev, enc->audio_queue);
	if (ret != 0) {
		LOG_ERR("Failed to configure audio source I2S, err %d", ret);
		audio_encoder_delete(enc);
		return NULL;
	}

	uint32_t const lc3_duration =
		params->frame_duration_us == 10000 ? FRAME_DURATION_10_MS : FRAME_DURATION_7_5_MS;

	/* Configure LC3 codec and allocate required memory */
	ret = lc3_api_configure(&enc->lc3_cfg, params->sampling_rate_hz, lc3_duration);
	if (ret) {
		LOG_ERR("Failed to configure LC3 codec, err %d", ret);
		audio_encoder_delete(enc);
		return NULL;
	}

	enc->lc3_scratch = malloc(lc3_api_encoder_scratch_size(&enc->lc3_cfg));
	if (!enc->lc3_scratch) {
		LOG_ERR("Failed to allocate encoder scratch memory");
		audio_encoder_delete(enc);
		return NULL;
	}

	for (int i = 0; i < ARRAY_SIZE(enc->channel); i++) {
		lc3_encoder_t *lc3_encoder;

		enc->channel[i].lc3_encoder = lc3_encoder = malloc(sizeof(*lc3_encoder));

		if (!lc3_encoder) {
			LOG_ERR("Failed to allocate LC3 encoder");
			audio_encoder_delete(enc);
			return NULL;
		}

		ret = lc3_api_initialise_encoder(&enc->lc3_cfg, lc3_encoder);
		if (ret) {
			LOG_ERR("Failed to initialise LC3 encoder %d, err %d", i, ret);
			audio_encoder_delete(enc);
			return NULL;
		}
	}

	/* Create and start thread */
	enc->tid = k_thread_create(&enc->thread, encoder_stack, CONFIG_LC3_ENCODER_STACK_SIZE,
				   audio_encoder_thread_func, enc, NULL, NULL,
				   CONFIG_ALIF_BLE_HOST_THREAD_PRIORITY + 1, 0, K_NO_WAIT);
	if (!enc->tid) {
		LOG_ERR("Failed to create encoder thread");
		audio_encoder_delete(enc);
		return NULL;
	}

	k_thread_name_set(enc->tid, "lc3_encoder");

	return enc;
}

struct audio_queue *audio_encoder_audio_queue_get(struct audio_encoder *encoder)
{
	if (!encoder) {
		return NULL;
	}
	return encoder->audio_queue;
}

int audio_encoder_add_channel(struct audio_encoder *const encoder, size_t const octets_per_frame,
			      uint32_t const stream_id)
{
	if (!encoder) {
		return -EINVAL;
	}

	LOG_DBG("Adding channel %u", stream_id);

	int const ch_index = alloc_channel_index(encoder);

	if (ch_index < 0) {
		LOG_ERR("No free channel slots");
		return ch_index;
	}

	encoder->channel[ch_index].enabled = false;
	encoder->channel[ch_index].stream_id = stream_id;

	struct sdu_queue *queue = encoder->channel[ch_index].sdu_queue;
	struct iso_datapath_htoc *iso_dp = encoder->channel[ch_index].iso_dp;

	sdu_queue_delete(queue);
	iso_datapath_htoc_delete(iso_dp);

	encoder->channel[ch_index].sdu_queue = queue =
		sdu_queue_create(CONFIG_ALIF_BLE_AUDIO_SDU_QUEUE_LENGTH, octets_per_frame);
	if (!queue) {
		LOG_ERR("Failed to create SDU queue (index %u)", stream_id);
		return -ENOMEM;
	}

	encoder->channel[ch_index].iso_dp = iso_dp =
		iso_datapath_htoc_init(stream_id, queue, 0 /*stream_id == 0*/);
	if (!iso_dp) {
		LOG_ERR("Failed to create ISO datapath (index %u)", stream_id);
		return -ENOMEM;
	}

	return 0;
}

int audio_encoder_start_channel(struct audio_encoder *const encoder, uint32_t const stream_id)
{
	if (!encoder) {
		return -EINVAL;
	}

	LOG_DBG("Starting channel %u", stream_id);

	int const ch_index = get_channel_index(encoder, stream_id);

	if (ch_index < 0) {
		LOG_ERR("Stream ID not found");
		return ch_index;
	}

	int ret = iso_datapath_htoc_bind(encoder->channel[ch_index].iso_dp);

	if (ret) {
		LOG_ERR("Failed to bind ISO datapath (index %u)", stream_id);
		return ret;
	}

	encoder->channel[ch_index].enabled = true;

	audio_source_i2s_start();

	return 0;
}

int audio_encoder_stop_channel(struct audio_encoder *const encoder, uint32_t const stream_id)
{
	if (!encoder) {
		return -EINVAL;
	}

	LOG_DBG("Stopping channel %u", stream_id);

	int const ch_index = get_channel_index(encoder, stream_id);

	if (ch_index < 0) {
		LOG_ERR("Stream ID not found");
		return ch_index;
	}

	encoder->channel[ch_index].enabled = false;

	iso_datapath_htoc_unbind(encoder->channel[ch_index].iso_dp);

	audio_source_i2s_stop();

	return 0;
}

int audio_encoder_register_cb(struct audio_encoder *const encoder, audio_encoder_sdu_cb_t const cb,
			      void *const context)
{
	if (!encoder || !cb) {
		return -EINVAL;
	}

	struct cb_list *cb_item = malloc(sizeof(*cb_item));

	if (!cb_item) {
		return -ENOMEM;
	}

	/* Insert callback in linked list */
	cb_item->cb = cb;
	cb_item->context = context;
	cb_item->next = encoder->cb_list;
	encoder->cb_list = cb_item;

	return 0;
}

int audio_encoder_delete(struct audio_encoder *encoder)
{
	if (!encoder) {
		return -EINVAL;
	}

	for (size_t iter = 0; iter < ARRAY_SIZE(encoder->channel); iter++) {
		audio_encoder_stop_channel(encoder, encoder->channel[iter].stream_id);
	}

	/* Signal to thread that it should abort */
	encoder->thread_abort = true;
	void *dummy_queue_item = NULL;

	k_msgq_put(&encoder->audio_queue->msgq, &dummy_queue_item, K_FOREVER);

	/* Join thread before freeing anything */
	k_thread_join(&encoder->thread, K_FOREVER);

	for (size_t iter = 0; iter < ARRAY_SIZE(encoder->channel); iter++) {
		iso_datapath_htoc_delete(encoder->channel[iter].iso_dp);
		sdu_queue_delete(encoder->channel[iter].sdu_queue);
		free(encoder->channel[iter].lc3_encoder);
	}

	free(encoder->lc3_scratch);

	audio_queue_delete(encoder->audio_queue);

	/* Free linked list of callbacks */
	while (encoder->cb_list) {
		struct cb_list *tmp = encoder->cb_list;

		encoder->cb_list = tmp->next;
		free(tmp);
	}

	free(encoder);

	return 0;
}
