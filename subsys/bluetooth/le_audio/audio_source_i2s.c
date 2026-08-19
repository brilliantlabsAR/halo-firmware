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
#include "audio_i2s_common.h"
#include "audio_source_i2s.h"

LOG_MODULE_REGISTER(audio_source_i2s, CONFIG_BLE_AUDIO_LOG_LEVEL);

#if CONFIG_ALIF_BLE_AUDIO_USE_RAMFUNC
#define INT_RAMFUNC __ramfunc
#else
#define INT_RAMFUNC
#endif

#if CONFIG_ALIF_BLE_AUDIO_FRAME_DURATION_10MS
#define FRAMES_PER_SECOND 100
#else
#error "Unsupported configuration"
#endif

#if CONFIG_ALIF_BLE_AUDIO_SOURCE_TRANSMISSION_DELAY_MS
#define TRANSMISSION_DELAY_US (CONFIG_ALIF_BLE_AUDIO_SOURCE_TRANSMISSION_DELAY_MS * 1000)
#else
#define TRANSMISSION_DELAY_US 0
#endif

/* Max supported sampling rate is 48kHz.
 * 10ms frame has 480 bytes and 7.5ms has 360 bytes.
 */
#define MAX_SAMPLES_PER_BLOCK 480
/* Left and right audio channels. */
#define NUMBER_OF_CHANNELS    2

#include <zephyr/drivers/gpio.h>

#define GPIO_TEST0_NODE DT_ALIAS(source_i2s_test0)
#define GPIO_TEST1_NODE DT_ALIAS(source_i2s_test1)

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
	LOG_INF("I2S source test pin %u initialized", p_pin->pin);
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
static int audio_source_i2s_init(void)
{
	init_test_pin(&test_pin0);
	init_test_pin(&test_pin1);
	return 0;
}
SYS_INIT(audio_source_i2s_init, APPLICATION, 0);

#endif

struct audio_source_i2s {
	const struct device *dev;
	struct audio_queue *audio_queue;
	struct audio_i2s_timing timing;
	size_t block_samples;
	size_t number_of_channels;
	bool drop_next_audio_block;
	bool ping_pong_buffer;
	bool started;
};
static struct audio_source_i2s audio_source;

struct audio_input_buffer {
	uint32_t timestamp;
	pcm_sample_t buf[NUMBER_OF_CHANNELS * MAX_SAMPLES_PER_BLOCK];
};
/** Ping pong input buffer for 16-bit PCM samples and two channels */
static struct audio_input_buffer samples_input_buffer[2];

struct last_block_job {
	struct k_work work;
	struct audio_input_buffer *p_block;
};

K_KERNEL_STACK_DEFINE(i2s_worker_stack, 2048);
static struct k_work_q i2s_worker_queue;

INT_RAMFUNC static void finish_last_block(struct k_work *work)
{
	struct last_block_job *p_context = CONTAINER_OF(work, struct last_block_job, work);
	struct audio_input_buffer *p_block = p_context->p_block;

#if DT_NODE_EXISTS(GPIO_TEST0_NODE)
	set_test_pin(&test_pin0, 1);
#endif

	struct audio_block *p_audiobuf = NULL;
	int ret =
		k_mem_slab_alloc(&audio_source.audio_queue->slab, (void **)&p_audiobuf, K_NO_WAIT);

	if (ret || !p_audiobuf) {
		/* No buffer available, just drop it */
		/* LOG_ERR("Audio queue is empty, dropping frame"); */
#if DT_NODE_EXISTS(GPIO_TEST0_NODE)
		set_test_pin(&test_pin0, 0);
#endif
		return;
	}

	bool const has_right_channel = audio_source.number_of_channels > 1;

	/* Populate the capture timestamp of the block */
	p_audiobuf->timestamp = p_block->timestamp;
	p_audiobuf->num_channels = 1 + has_right_channel;

#if CONFIG_I2S_SYNC_BUFFER_FORMAT_SEQUENTIAL
	size_t const block_samples = audio_source.block_samples;

	size_t const num_of_block_bytes = block_samples * sizeof(p_block->buf[0]);
	/* Copy left buffer */
	memcpy(p_audiobuf->channels[0], p_block->buf, num_of_block_bytes);
#if CONFIG_ALIF_BLE_AUDIO_NMB_CHANNELS > 1
	if (has_right_channel) {
		/* Copy right buffer */
		memcpy(p_audiobuf->channels[1], p_block->buf + block_samples, num_of_block_bytes);
	}
#endif

#else /* CONFIG_I2S_SYNC_BUFFER_FORMAT_SEQUENTIAL */
	size_t const input_block_samples = audio_source.timing.samples_per_block;

	pcm_sample_t const *p_input = p_block->buf;
	/* Loop input samples and copy sequentially.
	 * Every even sample is for left channel.
	 */
	pcm_sample_t *p_out_left = p_audiobuf->channels[0];
#if CONFIG_ALIF_BLE_AUDIO_NMB_CHANNELS > 1
	pcm_sample_t *p_out_right = p_audiobuf->channels[1];
#endif
	for (size_t iter = 0; iter < input_block_samples; iter++) {
		if (likely(has_right_channel) && iter & 1) {
#if CONFIG_ALIF_BLE_AUDIO_NMB_CHANNELS > 1
			*p_out_right++ = *p_input++;
#endif
			continue;
		}
		*p_out_left++ = *p_input++;
	}
#endif /* CONFIG_I2S_SYNC_BUFFER_FORMAT_SEQUENTIAL */

	if (k_msgq_put(&audio_source.audio_queue->msgq, &p_audiobuf, K_NO_WAIT)) {
		/* Failed to put into queue */
		k_mem_slab_free(&audio_source.audio_queue->slab, p_audiobuf);
		LOG_ERR("Audio msg queue is full, frame dropped");
	}
#if DT_NODE_EXISTS(GPIO_TEST0_NODE)
	set_test_pin(&test_pin0, 0);
#endif
}

static struct last_block_job finish_last_block_job = {
	.work = Z_WORK_INITIALIZER(finish_last_block),
	.p_block = NULL,
};

INT_RAMFUNC static void recv_next_block(const struct device *dev, uint32_t timestamp)
{
	bool const ping_pong = audio_source.ping_pong_buffer ^ true;

	audio_source.ping_pong_buffer = ping_pong;

	struct audio_input_buffer *const p_buffer = &samples_input_buffer[ping_pong];

	/* TODO: implement timing fix if needed... Always zero for now so ignore adjustment code! */
#if CORRECTION_SAMPLES_ENABLED
	const int32_t correction_samples = audio_i2s_get_sample_correction(NULL);

	/* Receive a full block by default */
	size_t rx_count = audio_source.timing.samples_per_block;
	size_t rx_offset = 0;

	if (correction_samples < 0) {
		/* Drop some samples, by receiving the amount of samples to drop and then dropping
		 * the whole buffer
		 */
		rx_count = -correction_samples;
		audio_source.drop_next_audio_block = true;
	} else if (correction_samples > 0) {
		/* Insert some zeros at the start of the current block, and receive into the rest of
		 * the block
		 */
		rx_count -= correction_samples;
		rx_offset = correction_samples;
	}

	if (rx_offset) {
		/* Fill any offset in the buffer with zeros, and adjust timestamp accordingly */
		memset(p_buffer->buf, 0, rx_offset * sizeof(p_buffer->buf[0]));
		timestamp -= audio_i2s_samples_to_us(correction_samples,
						     audio_source.timing.us_per_block,
						     audio_source.timing.samples_per_block);
	}
#else
	size_t const rx_count = audio_source.timing.samples_per_block;
	size_t const rx_offset = 0;
#endif

	i2s_sync_recv(dev, p_buffer->buf + rx_offset, rx_count * sizeof(p_buffer->buf[0]));

#if CONFIG_ALIF_BLE_AUDIO_SOURCE_TRANSMISSION_DELAY_ENABLED
	p_buffer->timestamp = timestamp + TRANSMISSION_DELAY_US;
#else
	p_buffer->timestamp = 0;
#endif
}

INT_RAMFUNC static void on_i2s_complete(const struct device *dev, enum i2s_sync_status status,
					void *block)
{
	/* Capture timestamp before doing anything else to reduce jitter */
	const uint32_t time_now = gapi_isooshm_dp_get_local_time();

#if DT_NODE_EXISTS(GPIO_TEST0_NODE)
	set_test_pin(&test_pin0, 1);
#endif

	recv_next_block(dev, time_now);

	if (audio_source.drop_next_audio_block || !block) {
		audio_source.drop_next_audio_block = false;
	} else {
		finish_last_block_job.p_block = CONTAINER_OF(block, struct audio_input_buffer, buf);
		k_work_submit_to_queue(&i2s_worker_queue, &finish_last_block_job.work);
	}

#if DT_NODE_EXISTS(GPIO_TEST0_NODE)
	set_test_pin(&test_pin0, 0);
#endif
}

int audio_source_i2s_configure(const struct device *dev, struct audio_queue *audio_queue)
{
	if (!dev || !audio_queue) {
		return -EINVAL;
	}

	/* TODO: Configure I2S here to match requested audio path configuration! */
	struct i2s_sync_config i2s_cfg;

	if (i2s_sync_get_config(dev, &i2s_cfg)) {
		LOG_ERR("Failed to get I2S config");
		return -EIO;
	}

	if (i2s_cfg.channel_count > NUMBER_OF_CHANNELS) {
		LOG_ERR("Invalid I2S channel count %u", i2s_cfg.channel_count);
		return -EINVAL;
	}

	i2s_cfg.sample_rate = audio_queue->sampling_freq_hz;
	if (i2s_sync_configure(dev, &i2s_cfg)) {
		LOG_ERR("Failed to configure I2S");
		return -EIO;
	}

	/* Calculate sample count per block depending on the frame duration (7.5ms or 10ms) */
	size_t const block_samples = audio_queue->frame_duration_us == 10000
					     ? (i2s_cfg.sample_rate * 10) / 1000
					     : (i2s_cfg.sample_rate * 75) / 10000;

	size_t const samples_per_full_block = i2s_cfg.channel_count * block_samples;

	if (samples_per_full_block > ARRAY_SIZE(samples_input_buffer[0].buf)) {
		LOG_ERR("Invalid I2S block size %u", samples_per_full_block);
		return -EINVAL;
	}

#if DT_NODE_EXISTS(GPIO_TEST0_NODE)
	set_test_pin(&test_pin0, 0);
#endif

	/* Shutdown existing stream and wait for start */
	i2s_sync_disable(dev, I2S_DIR_RX);

	audio_source.dev = dev;
	audio_source.audio_queue = audio_queue;
	audio_source.number_of_channels = i2s_cfg.channel_count;
	audio_source.block_samples = block_samples;
	audio_source.ping_pong_buffer = false;
	audio_source.started = false;
	audio_source.timing.correction_us = 0;
	audio_source.timing.us_per_block = audio_queue->frame_duration_us;
	audio_source.timing.samples_per_block = samples_per_full_block;

	/* Maximum positive correction is slightly less than a full audio block
	 * (cannot receive zero samples)
	 */
	audio_source.timing.max_single_correction = samples_per_full_block - 2;

	/* Minimum negative correction is one full audio block */
	audio_source.timing.min_single_correction = -samples_per_full_block;

	int ret = i2s_sync_register_cb(dev, I2S_DIR_RX, on_i2s_complete);

	if (ret) {
		LOG_ERR("Failed to register I2S callback");
		return ret;
	}

	static bool thread_started;

	if (thread_started) {
		return 0;
	}

	k_work_queue_start(&i2s_worker_queue, i2s_worker_stack,
			   K_KERNEL_STACK_SIZEOF(i2s_worker_stack),
			   CONFIG_ALIF_BLE_HOST_THREAD_PRIORITY - 1, NULL);
	k_thread_name_set(&i2s_worker_queue.thread, "audio_source_i2s");
	thread_started = true;

	return 0;
}

void audio_source_i2s_start(void)
{
	if (!audio_source.dev) {
		LOG_ERR("I2S source not configured");
		return;
	}

	if (audio_source.started) {
		return;
	}

	audio_source.started = true;

	/* Kick the I2S receive operation */
	const uint32_t time_now = gapi_isooshm_dp_get_local_time();

	recv_next_block(audio_source.dev, time_now);
}

void audio_source_i2s_stop(void)
{
	if (!audio_source.dev) {
		LOG_ERR("I2S source not configured");
		return;
	}

	if (!audio_source.started) {
		return;
	}

	i2s_sync_disable(audio_source.dev, I2S_DIR_RX);
	audio_source.started = false;
}

INT_RAMFUNC void audio_source_i2s_apply_timing_correction(int32_t const correction_us)
{
	audio_i2s_timing_apply_correction(&audio_source.timing, correction_us);
}
