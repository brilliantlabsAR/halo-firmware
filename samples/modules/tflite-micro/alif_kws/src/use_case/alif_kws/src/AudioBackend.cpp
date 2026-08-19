/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include "AudioBackend.hpp"

#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/i2s.h>

LOG_MODULE_REGISTER(AudioBackend);

BUILD_ASSERT(CONFIG_AUDIO_STRIDE % CONFIG_I2S_SAMPLES == 0,
	"CONFIG_AUDIO_STRIDE must be a multiple of CONFIG_I2S_SAMPLES");
BUILD_ASSERT(CONFIG_I2S_SAMPLES % 4 == 0, "CONFIG_I2S_SAMPLES must be a multiple of 4");

#define I2S_DEVICE              DT_ALIAS(i2s_mic)
#define I2S_SAMPLE_SIZE         sizeof(int16_t)
#define I2S_WORD_SIZE           (I2S_SAMPLE_SIZE * 8)
#define I2S_CHANNELS            CONFIG_I2S_CHANNELS
#define I2S_SAMPLES             CONFIG_I2S_SAMPLES
#define I2S_NUM_BUFFERS         CONFIG_I2S_NUM_BUFFERS
#define I2S_BUFFER_SIZE         (I2S_CHANNELS * I2S_SAMPLES * I2S_SAMPLE_SIZE)
#define I2S_GAIN                CONFIG_I2S_GAIN
#define I2S_THREAD_STACK_SIZE   CONFIG_I2S_THREAD_STACK_SIZE
#define I2S_THREAD_PRIORITY     CONFIG_I2S_THREAD_PRIORITY


K_MEM_SLAB_DEFINE_STATIC(mem_slab, I2S_BUFFER_SIZE, I2S_NUM_BUFFERS, 4);
K_THREAD_STACK_DEFINE(i2s_thread_stack, I2S_THREAD_STACK_SIZE);

static const struct device *i2s_mic = DEVICE_DT_GET(I2S_DEVICE);
static struct k_thread i2s_thread;
static struct k_sem rx_start;
static struct k_sem rx_ready;
static int16_t *user_ptr;
static int user_len;


static int i2s_mix_mono_output(int16_t *in, size_t in_size, int16_t *out, size_t out_size)
{
	size_t num_samples = in_size / 2;

	if (out_size < num_samples) {
		return -EINVAL;
	}

	for (size_t i = 0; i < num_samples; ++i) {
		int16_t left_channel = in[2 * i];
		int16_t right_channel = in[2 * i + 1];

		out[i] = (int16_t)((left_channel + right_channel) / 2);
	}

	return 0;
}

static int i2s_handle_rx(void)
{
	void *buffer = NULL;
	size_t size = 0;
	int offset = 0;

	while (offset < user_len) {
		int rc = i2s_read(i2s_mic, &buffer, &size);

		if (rc != 0) {
			LOG_ERR("i2s read failed: %i", rc);
			return rc;
		}

		int stero_samples = size / I2S_SAMPLE_SIZE;

		rc = i2s_mix_mono_output(static_cast<int16_t*>(buffer), stero_samples,
			user_ptr + offset, user_len - offset);
		if (rc < 0) {
			k_mem_slab_free(&mem_slab, buffer);
			LOG_ERR("i2s_mix_mono_output failed: %i", rc);
			return rc;
		}

		offset += stero_samples / 2;
		k_mem_slab_free(&mem_slab, buffer);
	}

	return 0;
}

static void i2s_worker_thread(void*, void*, void*)
{
	int rc = i2s_trigger(i2s_mic, I2S_DIR_RX, I2S_TRIGGER_START);

	if (rc < 0) {
		LOG_ERR("I2S_TRIGGER_START failed: %i", rc);
		return;
	}

	while (1) {
		k_sem_take(&rx_start, K_FOREVER);
		if (user_ptr == NULL) {
			break;
		}

		rc = i2s_handle_rx();
		if (rc < 0) {
			LOG_ERR("i2s_handle_rx failed: %i", rc);
			break;
		}

		k_sem_give(&rx_ready);
	}

	rc = i2s_trigger(i2s_mic, I2S_DIR_RX, I2S_TRIGGER_DROP);
	if (rc < 0) {
		LOG_ERR("I2S_TRIGGER_DROP failed: %i", rc);
	}
}

int audio_init(int sampling_rate)
{
	if (!device_is_ready(i2s_mic)) {
		LOG_ERR("i2s_mic is not ready");
		return -ENODEV;
	}

	const struct i2s_config config = {
		.word_size = I2S_WORD_SIZE,
		.channels = I2S_CHANNELS,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_FRAME_CLK_MASTER | I2S_OPT_BIT_CLK_MASTER,
		.frame_clk_freq = static_cast<uint32_t>(sampling_rate),
		.mem_slab = &mem_slab,
		.block_size = I2S_BUFFER_SIZE,
		.timeout = SYS_FOREVER_MS,
	};

	int rc = i2s_configure(i2s_mic, I2S_DIR_RX, &config);

	if (rc < 0) {
		LOG_ERR("i2s_configure failed: %i", rc);
		return rc;
	}

	k_sem_init(&rx_start, 0, 1);
	k_sem_init(&rx_ready, 0, 1);

	k_thread_create(&i2s_thread, i2s_thread_stack,
							K_THREAD_STACK_SIZEOF(i2s_thread_stack),
							i2s_worker_thread,
							NULL, NULL, NULL,
							I2S_THREAD_PRIORITY, 0, K_NO_WAIT);

	k_thread_name_set(&i2s_thread, "i2s audio");

	return 0;
}

void audio_uninit(void)
{
	user_ptr = NULL;
	user_len = 0;
	k_sem_give(&rx_start);
	k_thread_join(&i2s_thread, K_FOREVER);
}

int get_audio_data(int16_t *data, int len)
{
	user_ptr = data;
	user_len = len;
	k_sem_give(&rx_start);

	return 0;
}

int wait_for_audio(void)
{
	k_sem_take(&rx_ready, K_FOREVER);

	return 0;
}

void audio_preprocessing(int16_t *data, int len)
{
	for (int i = 0; i < len; ++i) {
		data[i] = data[i] * I2S_GAIN;
	}
}
