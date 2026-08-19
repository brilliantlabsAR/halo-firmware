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
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/util.h>
#include "drivers/i2s_sync.h"
#include "gapi_isooshm.h"
#include "presentation_compensation.h"
#include "audio_i2s_common.h"
#include "audio_sink_i2s.h"

LOG_MODULE_REGISTER(audio_sink_i2s, CONFIG_BLE_AUDIO_LOG_LEVEL);

#if CONFIG_ALIF_BLE_AUDIO_USE_RAMFUNC
#define INT_RAMFUNC __ramfunc
#else
#define INT_RAMFUNC
#endif

#include <zephyr/drivers/gpio.h>

#define GPIO_TEST0_NODE DT_ALIAS(sink_i2s_test0) /* DT_NODELABEL(test_pin0) */
#define GPIO_TEST1_NODE DT_ALIAS(sink_i2s_test1) /* DT_NODELABEL(test_pin1) */

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

/* Initialisation to perform pre-main */
static int audio_sink_i2s_init(void)
{
	init_test_pin(&test_pin0);
	init_test_pin(&test_pin1);
	return 0;
}
SYS_INIT(audio_sink_i2s_init, APPLICATION, 0);
#endif

struct audio_sink_i2s {
	const struct device *dev;
	struct audio_queue *audio_queue;
	struct audio_block *current_block;
	bool awaiting_buffer;

	struct audio_i2s_timing timing;
};

struct pres_delay_work {
	struct k_work work;
	uint32_t pres_delay_us;
};

static struct audio_sink_i2s audio_sink;
static struct pres_delay_work pd_work;

static pcm_sample_t silence[MAX_SAMPLES_PER_AUDIO_BLOCK];

INT_RAMFUNC static void send_next_block(const struct device *dev, uint32_t const time_now)
{
	struct audio_block *block = NULL;
	int32_t const correction_samples = audio_i2s_get_sample_correction(&audio_sink.timing);

	/* Send required size of silence and return */
	if (correction_samples > 0) {
#if DT_NODE_EXISTS(GPIO_TEST1_NODE)
		set_test_pin(&test_pin1, 1);
#endif
		i2s_sync_send(dev, silence, correction_samples * sizeof(silence[0]));
		return;
	}

	int ret = k_msgq_get(&audio_sink.audio_queue->msgq, &block, K_NO_WAIT);

	if (ret || !block) {
		/* If there is no available buffer, disable I2S transmitter and flag waiting for
		 * data
		 */
		i2s_sync_disable(dev, I2S_DIR_TX);
		audio_sink.awaiting_buffer = true;
		return;
	}

#if DT_NODE_EXISTS(GPIO_TEST0_NODE)
	set_test_pin(&test_pin0, 1);
#endif

	size_t tx_count = audio_sink.timing.samples_per_block;
	size_t tx_offset = 0;
	uint32_t pres_delay_offset = 0;

	/* If necessary drop some samples from the start of the buffer */
	if (correction_samples < 0) {
		tx_count += correction_samples;
		tx_offset = -correction_samples;
		pres_delay_offset =
			audio_i2s_samples_to_us(-correction_samples, audio_sink.timing.us_per_block,
						audio_sink.timing.samples_per_block);
	}

	i2s_sync_send(dev, block->buf_left + tx_offset, tx_count * sizeof(pcm_sample_t));

	/* Calculate presentation delay, and then sumbit a work item to perform presentation
	 * compensation calculations so that this is deferred and not performed in ISR context
	 */
	pd_work.pres_delay_us = time_now - block->timestamp - pres_delay_offset;
	k_work_submit(&pd_work.work);

	audio_sink.current_block = block;
}

INT_RAMFUNC static void on_i2s_complete(const struct device *dev, enum i2s_sync_status status,
					void *p_block)
{
	ARG_UNUSED(p_block);

#if DT_NODE_EXISTS(GPIO_TEST0_NODE)
	set_test_pin(&test_pin0, 0);
#endif
#if DT_NODE_EXISTS(GPIO_TEST1_NODE)
	set_test_pin(&test_pin1, 0);
#endif

	/* Capture timestamp before doing anything else to reduce jitter */
	uint32_t const time_now = gapi_isooshm_dp_get_local_time();
	struct audio_block *const block = audio_sink.current_block;

	audio_sink.current_block = NULL;

	send_next_block(dev, time_now);

	if (block) {
		k_mem_slab_free(&audio_sink.audio_queue->slab, block);
	}
}

INT_RAMFUNC static void submit_presentation_delay(struct k_work *item)
{
	(void)item;
	/* struct pres_delay_work *w = CONTAINER_OF(item, struct pres_delay_work, work); */

	/* presentation_compensation_notify_timing(w->pres_delay_us); */
}

int audio_sink_i2s_configure(const struct device *dev, struct audio_queue *audio_queue)
{
	if ((dev == NULL) || (audio_queue == NULL)) {
		return -EINVAL;
	}

	struct i2s_sync_config i2s_cfg;

	if (i2s_sync_get_config(dev, &i2s_cfg)) {
		LOG_ERR("Failed to get I2S config");
		return -EIO;
	}

	i2s_cfg.sample_rate = audio_queue->sampling_freq_hz;

	if (i2s_sync_configure(dev, &i2s_cfg)) {
		LOG_ERR("Failed to configure I2S");
		return -EIO;
	}

	size_t const samples_per_full_block =
		i2s_cfg.channel_count * audio_queue->audio_block_samples;

	audio_sink.dev = dev;
	audio_sink.audio_queue = audio_queue;
	audio_sink.timing.correction_us = 0;
	audio_sink.timing.us_per_block = audio_queue->frame_duration_us;
	audio_sink.timing.samples_per_block = samples_per_full_block;

	/* Maximum positive correction is the size of the silence buffer */
	audio_sink.timing.max_single_correction = ARRAY_SIZE(silence);

	/* Minimum negative correction is slightly less than a full audio block (cannot send zero
	 * samples)
	 */
	audio_sink.timing.min_single_correction = 2 - (int32_t)samples_per_full_block;

	k_work_init(&pd_work.work, submit_presentation_delay);

	int ret = i2s_sync_register_cb(dev, I2S_DIR_TX, on_i2s_complete);

	if (ret) {
		LOG_ERR("Failed to register I2S callback");
		return ret;
	}

	/* Flag that audio sink has not started yet */
	audio_sink.awaiting_buffer = true;

#if DT_NODE_EXISTS(GPIO_TEST0_NODE)
	set_test_pin(&test_pin0, 0);
#endif
#if DT_NODE_EXISTS(GPIO_TEST1_NODE)
	set_test_pin(&test_pin1, 0);
#endif

	return 0;
}

INT_RAMFUNC void audio_sink_i2s_notify_buffer_available(void *param, uint32_t const timestamp,
							uint16_t const sdu_seq)
{
	(void)param;
	(void)timestamp;
	(void)sdu_seq;

	if (!audio_sink.awaiting_buffer) {
		return;
	}

	uint32_t time_now = gapi_isooshm_dp_get_local_time();

	audio_sink.awaiting_buffer = false;
	send_next_block(audio_sink.dev, time_now);
}

INT_RAMFUNC void audio_sink_i2s_apply_timing_correction(int32_t correction_us)
{
	audio_i2s_timing_apply_correction(&audio_sink.timing, correction_us);
}
