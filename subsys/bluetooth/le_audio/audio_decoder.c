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

#include <stdlib.h>

#include "alif_lc3.h"
#include "lc3_api.h"
#include "audio_queue.h"
#include "sdu_queue.h"
#include "gapi_isooshm.h"
#include "audio_decoder.h"

#include "bluetooth/le_audio/audio_sink_i2s.h"
#include "bluetooth/le_audio/iso_datapath_ctoh.h"

#if CONFIG_ALIF_BLE_AUDIO_USE_RAMFUNC
#define INT_RAMFUNC __ramfunc
#else
#define INT_RAMFUNC
#endif

#define AUDIO_QUEUE_MARGIN_US     (CONFIG_ALIF_BLE_AUDIO_PRESENTATION_DELAY_QUEUE_MARGIN * 1000)
#define MIN_PRESENTATION_DELAY_US (CONFIG_ALIF_BLE_AUDIO_MIN_PRESENTATION_DELAY_MS * 1000)

/* Send same input data to both channels if one channel is not present.
 * This might happen at the start of the streams.
 */
#define SEND_SAME_DATA_IN_START_UP 1

LOG_MODULE_REGISTER(audio_decoder, CONFIG_BLE_AUDIO_LOG_LEVEL);

#include <zephyr/drivers/gpio.h>

#define GPIO_TEST0_NODE DT_ALIAS(decoder_test0) /* DT_NODELABEL(test_pin0) */
#define GPIO_TEST1_NODE DT_ALIAS(decoder_test1) /* DT_NODELABEL(test_pin1) */

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
	LOG_INF("Test pin %u initialized", p_pin->pin);
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
static int audio_decoder_init(void)
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
SYS_INIT(audio_decoder_init, APPLICATION, 0);

struct cb_list {
	audio_decoder_sdu_cb_t cb;
	void *context;
	struct cb_list *next;
};

struct channel_data {
	/* Input and output queues */
	struct sdu_queue *sdu_queue;
	struct iso_datapath_ctoh *iso_dp;
	lc3_decoder_t *lc3_decoder;
	int32_t *lc3_status;
	uint32_t stream_id;
	bool enabled;
};

struct audio_decoder {
	volatile bool thread_abort;
	struct audio_queue *audio_queue;
	struct channel_data channel[CONFIG_ALIF_BLE_AUDIO_NMB_CHANNELS];
	/* LC3 configuration, decoder instances and memory */
	lc3_cfg_t lc3_cfg;
	int32_t *lc3_scratch;
	/* Linked list of registered callbacks */
	struct cb_list *cb_list;
	/* Decoder thread */
	struct k_thread thread;
	k_tid_t tid;
};

K_THREAD_STACK_DEFINE(decoder_stack, CONFIG_LC3_DECODER_STACK_SIZE);

#if !CONFIG_I2S_SYNC_BUFFER_FORMAT_SEQUENTIAL
/* Audio output data must be in "interleaved" format meaning that every even pcm data is left
 * channel and every odd is right channel.
 * Temporary buffer is needed to handle LC3 output per input channel.
 */
static pcm_sample_t pcm_temp_buffer[CONFIG_ALIF_BLE_AUDIO_NMB_CHANNELS]
				   [MAX_SAMPLES_PER_AUDIO_BLOCK];
#endif

static int alloc_channel_index(struct audio_decoder const *const decoder)
{
	for (size_t iter = 0; iter < ARRAY_SIZE(decoder->channel); iter++) {
		if (decoder->channel[iter].stream_id == UINT32_MAX) {
			return iter;
		}
	}
	return -EINVAL;
}

static int get_channel_index(struct audio_decoder const *const decoder, uint32_t stream_id)
{
	for (size_t iter = 0; iter < ARRAY_SIZE(decoder->channel); iter++) {
		if (decoder->channel[iter].stream_id == stream_id) {
			return iter;
		}
	}

	return -EINVAL;
}

INT_RAMFUNC static void audio_decoder_thread_func(void *p1, void *p2, void *p3)
{
	struct audio_decoder *dec = (struct audio_decoder *)p1;
	(void)p2;
	(void)p3;

	LOG_DBG("Decoder thread started");

	int ret;
	struct audio_block *audio;
	gapi_isooshm_sdu_buf_t *p_sdu;
	struct audio_queue *const audio_queue = dec->audio_queue;
	size_t const audio_block_samples = audio_queue->audio_block_samples;
	size_t iter, num_channels;
	size_t last_sdu_seq = 0;
	uint32_t timestamp;
	uint8_t bec_detect;

#if DT_NODE_EXISTS(GPIO_TEST0_NODE)
	set_test_pin(&test_pin0, 0);
#endif
#if DT_NODE_EXISTS(GPIO_TEST1_NODE)
	set_test_pin(&test_pin1, 0);
#endif

#define LEFT_CH  (1 << 0)
#define RIGHT_CH (1 << 1)

	while (!dec->thread_abort) {
		/* Get a free audio block to decode into */
		audio = NULL;
		ret = k_mem_slab_alloc(&audio_queue->slab, (void **)&audio, K_FOREVER);
		if (ret || !audio) {
			k_sleep(K_MSEC(2));
			LOG_ERR("Failed to allocate audio block");
			continue;
		}

		timestamp = 0;
		num_channels = 0;

get_next_sdus:

		iter = ARRAY_SIZE(dec->channel);

		while (iter--) {
			struct channel_data *channel = &dec->channel[iter];

			if (!channel->sdu_queue || !channel->enabled) {
				continue;
			}

			/* Get an SDU */
			p_sdu = NULL;

			ret = k_msgq_get(&channel->sdu_queue->msgq, &p_sdu, K_NO_WAIT);
			if (ret || !p_sdu) {
				continue;
			}

			/* A NULL SDU can be sent to the queue to wake up and abort the
			 * thread. Stop processing for this loop and check thread abort
			 * flag.
			 */
			if (!p_sdu) {
				if (dec->thread_abort) {
					LOG_DBG("Decoder thread aborting");
					goto decode_finalize;
				}
				continue;
			}

#if CONFIG_I2S_SYNC_BUFFER_FORMAT_SEQUENTIAL
			/* Left channel should be decoded into first half of audio buffer,
			 * right channel into second half
			 */
			pcm_sample_t *const p_audio_data =
				audio->buf_left + audio_block_samples * iter;
#else
			pcm_sample_t *const p_audio_data = pcm_temp_buffer[iter];
#endif

			bool const bad_frame = (p_sdu->status != GAPI_ISOOSHM_SDU_STATUS_VALID);

			/* Keep for debugging. Don't enable by default to avoid timing issue.
			if (bad_frame) {
				LOG_WRN("Bad frame received, SDU status: %u", p_sdu->status);
			}
			*/

#if DT_NODE_EXISTS(GPIO_TEST0_NODE)
			set_test_pin(&test_pin0, 1);
#endif
			ret = lc3_api_decode_frame(&dec->lc3_cfg, channel->lc3_decoder, p_sdu->data,
						   p_sdu->sdu_len, bad_frame, &bec_detect,
						   p_audio_data, dec->lc3_scratch);
#if DT_NODE_EXISTS(GPIO_TEST0_NODE)
			set_test_pin(&test_pin0, 0);
#endif
			if (ret) {
				LOG_ERR("LC3 decoding failed on channel %d with err %d", iter, ret);
				continue;
			}

			if (bec_detect || bad_frame) {
				/* Keep for debugging. Don't enable by default to avoid timing
				 * issue. LOG_WRN("Corrupted input frame is detected [%u]", iter);
				 */
			} else {
				num_channels |= (1 << iter);
				timestamp = p_sdu->timestamp;
				last_sdu_seq = p_sdu->seq_num;
			}

			/* SDU is no longer needed, free it */
			k_mem_slab_free(&channel->sdu_queue->slab, p_sdu);
		}

		if (!num_channels) {
			if (dec->thread_abort) {
				break;
			}
			/* No channels present, wait for next block */
			k_sleep(K_MSEC(1));
			goto get_next_sdus;
		}

#if CONFIG_I2S_SYNC_BUFFER_FORMAT_SEQUENTIAL
		if (num_channels != (LEFT_CH + RIGHT_CH)) {
#if SEND_SAME_DATA_IN_START_UP
			pcm_sample_t *p_dst, *p_src;

			if (num_channels & LEFT_CH) {
				p_src = audio->buf_left;
				p_dst = audio->buf_left + audio_block_samples;
			} else {
				p_src = audio->buf_left + audio_block_samples;
				p_dst = audio->buf_left;
			}
			memcpy(p_dst, p_src, audio_block_samples * sizeof(pcm_sample_t));

#else /* !SEND_SAME_DATA_IN_START_UP */
			if (num_channels & LEFT_CH) {
				pcm_sample_t *p_dst = audio->buf_left + audio_block_samples;

#if ALIF_BLE_AUDIO_NMB_CHANNELS == 1
				/* If right channel is not present, copy left channel data aka mono
				 * mode
				 */
				memcpy(p_dst, audio->buf_left,
				       audio_block_samples * sizeof(pcm_sample_t));
#else
				/* Silence right channel data */
				memset(p_dst, 0, audio_block_samples * sizeof(pcm_sample_t));
#endif
			} else {
				/* Silence left channel data */
				memset(audio->buf_left, 0,
				       audio_block_samples * sizeof(pcm_sample_t));
			}
#endif /* SEND_SAME_DATA_IN_START_UP */
		}

#else /* !CONFIG_I2S_SYNC_BUFFER_FORMAT_SEQUENTIAL */
		/* fill the audio buffer with proper data format */
		pcm_sample_t *p_audio_out = audio->buf_left;

		if (num_channels != (LEFT_CH + RIGHT_CH)) {
			pcm_sample_t sample;
			pcm_sample_t const *p_in =
				(num_channels & LEFT_CH) ? pcm_temp_buffer[0] : pcm_temp_buffer[1];

			iter = audio_block_samples;
			while (iter--) {
				sample = *p_in++;
				*p_audio_out++ = sample;
				*p_audio_out++ = sample;
			}
		} else {
			for (size_t sample = 0; sample < audio_block_samples; sample++) {
				*p_audio_out++ = pcm_temp_buffer[0][sample];
#if CONFIG_ALIF_BLE_AUDIO_NMB_CHANNELS > 1
				*p_audio_out++ = pcm_temp_buffer[1][sample];
#endif
			}
		}

#endif /* CONFIG_I2S_SYNC_BUFFER_FORMAT_SEQUENTIAL */

#if DT_NODE_EXISTS(GPIO_TEST1_NODE)
		if ((last_sdu_seq % 128) == 0) {
			toggle_test_pin(&test_pin1);
		}
#endif

decode_finalize:
		audio->timestamp = timestamp;
		audio->num_channels = (num_channels == (LEFT_CH + RIGHT_CH)) ? 2 : 1;

		/* Push the audio data to queue */
		ret = k_msgq_put(&audio_queue->msgq, (void **)&audio, K_FOREVER);
		if (ret) {
			k_sleep(K_MSEC(1));
			LOG_ERR("Failed to push audio block to queue");
			/* try again */
			goto decode_finalize;
		}

		/* Notify I2S sink that it has a buffer available */
		audio_sink_i2s_notify_buffer_available(NULL, 0, 0);

		/* Notify datapath that SDUs are completed. This also triggers next read
		 * if last one was failed for some reason.
		 */
		for (int i = 0; i < ARRAY_SIZE(dec->channel); i++) {
			iso_datapath_ctoh_notify_sdu_done(dec->channel[i].iso_dp, timestamp,
							  last_sdu_seq);
		}

		/* Notify listeners that a block is completed */
		struct cb_list *cb_item = dec->cb_list;
		while (cb_item) {
			cb_item->cb(cb_item->context, timestamp, last_sdu_seq);
			cb_item = cb_item->next;
		}
	}

	LOG_DBG("Decoder thread finished");
}

struct audio_decoder *audio_decoder_create(struct audio_decoder_params const *const params)
{
	struct audio_decoder *dec;
	int ret;

	if (!params || !params->i2s_dev) {
		return NULL;
	}

	if (ARRAY_SIZE(dec->channel) < params->num_queues) {
		LOG_ERR("Too many queues!");
		return NULL;
	}

	dec = calloc(1, sizeof(*dec));

	if (!dec) {
		LOG_ERR("Failed to allocate audio decoder");
		return NULL;
	}

	size_t iter = params->num_queues;

	while (iter--) {
		dec->channel[iter].sdu_queue = params->p_sdu_queues[iter];
	}

	for (size_t iter = 0; iter < ARRAY_SIZE(dec->channel); iter++) {
		dec->channel[iter].stream_id = UINT32_MAX;
	}

	/* Presentation delay less than a certain value is impossible due to latency of audio
	 * datapath
	 */
	uint32_t const pres_delay_us = MIN(params->pres_delay_us, MIN_PRESENTATION_DELAY_US);
	size_t const audio_queue_len_blocks =
		1 + (pres_delay_us + AUDIO_QUEUE_MARGIN_US) / params->frame_duration_us;

	dec->audio_queue = audio_queue_create(audio_queue_len_blocks, params->sampling_rate_hz,
					      params->frame_duration_us);

	if (!dec->audio_queue) {
		free(dec);
		LOG_ERR("Failed to create audio queue");
		return NULL;
	}

	ret = audio_sink_i2s_configure(params->i2s_dev, dec->audio_queue);
	if (ret != 0) {
		LOG_ERR("Failed to configure audio sink I2S, err %d", ret);
		audio_decoder_delete(dec);
		return NULL;
	}

	uint32_t const lc3_duration =
		params->frame_duration_us == 10000 ? FRAME_DURATION_10_MS : FRAME_DURATION_7_5_MS;

	/* Configure LC3 codec and allocate required memory */
	ret = lc3_api_configure(&dec->lc3_cfg, params->sampling_rate_hz, lc3_duration);

	if (ret) {
		LOG_ERR("Failed to configure LC3 codec, err %d", ret);
		audio_decoder_delete(dec);
		return NULL;
	}

	dec->lc3_scratch = malloc(lc3_api_decoder_scratch_size(&dec->lc3_cfg));
	if (!dec->lc3_scratch) {
		LOG_ERR("Failed to allocate decoder scratch memory");
		audio_decoder_delete(dec);
		return NULL;
	}

	size_t const status_size = lc3_api_decoder_status_size(&dec->lc3_cfg);
	lc3_decoder_t *lc3_decoder;
	void *lc3_status;

	for (int i = 0; i < ARRAY_SIZE(dec->channel); i++) {
		dec->channel[i].lc3_decoder = lc3_decoder = malloc(sizeof(*lc3_decoder));
		if (!lc3_decoder) {
			LOG_ERR("Failed to allocate LC3 decoder");
			audio_decoder_delete(dec);
			return NULL;
		}

		dec->channel[i].lc3_status = lc3_status = malloc(status_size);
		if (!lc3_status) {
			LOG_ERR("Failed to allocate LC3 status memory");
			audio_decoder_delete(dec);
			return NULL;
		}

		ret = lc3_api_initialise_decoder(&dec->lc3_cfg, lc3_decoder, lc3_status);
		if (ret) {
			LOG_ERR("Failed to initialise LC3 decoder %d, err %d", i, ret);
			audio_decoder_delete(dec);
			return NULL;
		}
	}

	/* Create and start thread */
	dec->tid = k_thread_create(&dec->thread, decoder_stack, CONFIG_LC3_DECODER_STACK_SIZE,
				   audio_decoder_thread_func, dec, NULL, NULL,
				   CONFIG_ALIF_BLE_HOST_THREAD_PRIORITY, 0, K_NO_WAIT);

	if (!dec->tid) {
		LOG_ERR("Failed to create decoder thread");
		audio_decoder_delete(dec);
		return NULL;
	}

	k_thread_name_set(dec->tid, "lc3_decoder");

	return dec;
}

int audio_decoder_add_channel(struct audio_decoder *const decoder, size_t const octets_per_frame,
			      uint32_t const stream_id)
{
	if (!decoder) {
		return -EINVAL;
	}

	LOG_DBG("Adding channel %u", stream_id);

	int const ch_index = alloc_channel_index(decoder);

	if (ch_index < 0) {
		return ch_index;
	}

	decoder->channel[ch_index].stream_id = stream_id;
	decoder->channel[ch_index].enabled = false;

	struct sdu_queue *queue = decoder->channel[ch_index].sdu_queue;
	struct iso_datapath_ctoh *iso_dp = decoder->channel[ch_index].iso_dp;

	sdu_queue_delete(queue);
	iso_datapath_ctoh_delete(iso_dp);

	decoder->channel[ch_index].sdu_queue = queue =
		sdu_queue_create(CONFIG_ALIF_BLE_AUDIO_SDU_QUEUE_LENGTH, octets_per_frame);
	if (!queue) {
		LOG_ERR("Failed to create SDU queue (index %u)", stream_id);
		return -ENOMEM;
	}

	decoder->channel[ch_index].iso_dp = iso_dp = iso_datapath_ctoh_init(stream_id, queue);
	if (!iso_dp) {
		LOG_ERR("Failed to create ISO datapath (index %u)", stream_id);
		return -ENOMEM;
	}

	return 0;
}

int audio_decoder_start_channel(struct audio_decoder *const decoder, size_t const stream_id)
{
	if (!decoder) {
		return -EINVAL;
	}

	LOG_DBG("Starting channel %u", stream_id);

	int const ch_index = get_channel_index(decoder, stream_id);

	if (ch_index < 0) {
		return ch_index;
	}

	decoder->channel[ch_index].enabled = true;

	int err = iso_datapath_ctoh_start(decoder->channel[ch_index].iso_dp);

	return err;
}

int audio_decoder_stop_channel(struct audio_decoder *const decoder, size_t const stream_id)
{
	if (!decoder) {
		return -EINVAL;
	}

	LOG_DBG("Stopping channel %u", stream_id);

	int const ch_index = get_channel_index(decoder, stream_id);

	if (ch_index < 0) {
		return ch_index;
	}

	int const err = iso_datapath_ctoh_stop(decoder->channel[ch_index].iso_dp);

	if (err) {
		return err;
	}

	decoder->channel[ch_index].enabled = false;

	return 0;
}

int audio_decoder_register_cb(struct audio_decoder *const decoder, audio_decoder_sdu_cb_t const cb,
			      void *const context)
{
	if (!decoder || !cb) {
		return -EINVAL;
	}

	struct cb_list *cb_item = malloc(sizeof(*cb_item));

	if (!cb_item) {
		return -ENOMEM;
	}

	/* Insert callback in linked list */
	cb_item->cb = cb;
	cb_item->context = context;
	cb_item->next = decoder->cb_list;
	decoder->cb_list = cb_item;

	return 0;
}

int audio_decoder_delete(struct audio_decoder *decoder)
{
	if (!decoder) {
		return -EINVAL;
	}

	/* Signal to thread that it should abort */
	decoder->thread_abort = true;
	void *dummy_queue_item = NULL;

	/* Send dummy item to wake up and cancel thread */
	k_msgq_put(&decoder->channel[0].sdu_queue->msgq, &dummy_queue_item, K_FOREVER);

	/* Join thread before freeing anything */
	k_thread_join(&decoder->thread, K_FOREVER);

	for (int i = 0; i < ARRAY_SIZE(decoder->channel); i++) {
		free(decoder->channel[i].lc3_decoder);
		free(decoder->channel[i].lc3_status);
		iso_datapath_ctoh_delete(decoder->channel[i].iso_dp);
		sdu_queue_delete(decoder->channel[i].sdu_queue);
	}

	free(decoder->lc3_scratch);

	/* Free linked list of callbacks */
	while (decoder->cb_list) {
		struct cb_list *tmp = decoder->cb_list;

		decoder->cb_list = tmp->next;
		free(tmp);
	}

	audio_queue_delete(decoder->audio_queue);

	free(decoder);

	return 0;
}
