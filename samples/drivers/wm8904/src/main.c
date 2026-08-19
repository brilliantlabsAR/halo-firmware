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
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <math.h>

#define I2S_WORD_SIZE_BITS                16
#define I2S_WORD_SIZE_BYTES               (I2S_WORD_SIZE_BITS / 8)
#define I2S_CHANNEL_COUNT                 2
#define I2S_SAMPLE_FREQUENCY              48000
#define I2S_SAMPLES_PER_CHANNEL_PER_FRAME 480
#define I2S_TIMEOUT_MS                    SYS_FOREVER_MS
#define I2S_BLOCK_COUNT                   5
#define I2S_MEM_ALIGN                     4
#define I2S_BLOCK_SIZE_BYTES                                                                       \
	(I2S_SAMPLES_PER_CHANNEL_PER_FRAME * I2S_CHANNEL_COUNT * I2S_WORD_SIZE_BYTES)

#define SIGNAL_AMPLITUDE 4000.0f
#define SIGNAL_FREQUENCY 400.0f
#define PI               3.14159265358979323846f /* M_PI is missing from math.h */

LOG_MODULE_REGISTER(main);

#define CODEC_NODE DT_ALIAS(audio_codec)
static const struct device *codec_dev = DEVICE_DT_GET(CODEC_NODE);

#define I2S_NODE DT_ALIAS(i2s_bus)
static const struct device *i2s_dev = DEVICE_DT_GET(I2S_NODE);

K_MEM_SLAB_DEFINE_STATIC(i2s_mem, I2S_BLOCK_SIZE_BYTES, I2S_BLOCK_COUNT, I2S_MEM_ALIGN);

static int16_t tx_data_block[I2S_CHANNEL_COUNT * I2S_SAMPLES_PER_CHANNEL_PER_FRAME];

static void tx_data_populate(void)
{
	/* Populate the TX data with a constant frequency signal */
	for (uint32_t i = 0; i < I2S_SAMPLES_PER_CHANNEL_PER_FRAME; i++) {
		int16_t *left = &tx_data_block[i * I2S_CHANNEL_COUNT];
		int16_t *right = &tx_data_block[i * I2S_CHANNEL_COUNT + 1];

		int16_t sample = SIGNAL_AMPLITUDE *
				 sin((2.0f * PI * SIGNAL_FREQUENCY * i) / I2S_SAMPLE_FREQUENCY);
		*left = sample;
		*right = sample;
	}
}

static int write_one_block(const struct device *dev, k_timeout_t timeout)
{
	void *mem_block;

	int ret = k_mem_slab_alloc(&i2s_mem, &mem_block, timeout);

	if (ret) {
		LOG_ERR("Failed to allocate slab, err %d", ret);
		return ret;
	}

	memcpy(mem_block, tx_data_block, I2S_BLOCK_SIZE_BYTES);

	ret = i2s_write(dev, mem_block, I2S_BLOCK_SIZE_BYTES);
	if (ret) {
		LOG_ERR("Failed to write to I2S, err %d", ret);
		return ret;
	}

	return 0;
}

static int i2s_buffers_fill(const struct device *dev)
{
	for (uint32_t i = 0; i < I2S_BLOCK_COUNT; i++) {
		int ret = write_one_block(dev, K_NO_WAIT);

		if (ret) {
			return ret;
		}
	}

	return 0;
}

int main(void)
{
	LOG_INF("WM8904 demo starting");

	if (!device_is_ready(codec_dev)) {
		LOG_ERR("WM8904 codec is not ready");
		return -1;
	}

	if ((!device_is_ready(i2s_dev))) {
		LOG_ERR("I2S device is not ready");
		return -1;
	}

	tx_data_populate();

	struct i2s_config i2s_cfg = {.word_size = I2S_WORD_SIZE_BITS,
				     .channels = I2S_CHANNEL_COUNT,
				     .format = I2S_FMT_DATA_FORMAT_I2S,
				     .options = I2S_OPT_FRAME_CLK_MASTER | I2S_OPT_BIT_CLK_MASTER,
				     .frame_clk_freq = I2S_SAMPLE_FREQUENCY,
				     .mem_slab = &i2s_mem,
				     .block_size = I2S_BLOCK_SIZE_BYTES,
				     .timeout = I2S_TIMEOUT_MS};

	int ret = i2s_configure(i2s_dev, I2S_DIR_TX, &i2s_cfg);

	if (ret) {
		LOG_ERR("Failed to configure I2S, err %d", ret);
		return ret;
	}

	ret = i2s_buffers_fill(i2s_dev);
	if (ret) {
		LOG_ERR("Failed to fill I2S buffers, err %d", ret);
		return ret;
	}

	ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret) {
		LOG_ERR("Failed to trigger I2S, err %d", ret);
		return ret;
	}

	LOG_INF("Triggered I2S start");

	uint32_t ctr = 0;

	while (1) {
		/* Continuously fill the I2S buffers. */
		ret = write_one_block(i2s_dev, K_FOREVER);
		if (ret) {
			LOG_ERR("Failed to fill I2S buffer");
			return ret;
		}

		ctr++;

		if ((ctr % 128) == 0) {
			LOG_INF("I2S blocks sent: %u", ctr);
		}
	}
}
