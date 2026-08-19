#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/i2s.h>

#include "LiveMicInput.h"

LOG_MODULE_REGISTER(LiveMicInput);

BUILD_ASSERT(LiveMicInput::OutputSize % CONFIG_I2S_SAMPLES == 0,
	     "LiveMicInput::OutputSize must be a multiple of CONFIG_I2S_SAMPLES");
BUILD_ASSERT(CONFIG_I2S_SAMPLES % 4 == 0, "CONFIG_I2S_SAMPLES must be a multiple of 4");

#define I2S_DEVICE      DT_ALIAS(i2s_mic)
#define I2S_SAMPLE_SIZE sizeof(int16_t)
#define I2S_WORD_SIZE   (I2S_SAMPLE_SIZE * 8)
#define I2S_SAMPLE_RATE CONFIG_I2S_SAMPLE_RATE
#define I2S_CHANNELS    CONFIG_I2S_CHANNELS
#define I2S_SAMPLES     CONFIG_I2S_SAMPLES
#define I2S_NUM_BUFFERS CONFIG_I2S_NUM_BUFFERS
#define I2S_BUFFER_SIZE (I2S_CHANNELS * I2S_SAMPLES * I2S_SAMPLE_SIZE)
#define I2S_GAIN        CONFIG_I2S_GAIN

K_MEM_SLAB_DEFINE_STATIC(mem_slab, I2S_BUFFER_SIZE, I2S_NUM_BUFFERS, 4);
static const struct device *i2s_mic = DEVICE_DT_GET(I2S_DEVICE);

static int i2s_mix_mono_output(int16_t *in, size_t in_size, int16_t *out, size_t out_size)
{
	size_t num_samples = in_size / 2;

	if (out_size < num_samples) {
		return -EINVAL;
	}

	for (size_t i = 0; i < num_samples; ++i) {
		int16_t left_channel = in[2 * i];
		int16_t right_channel = in[2 * i + 1];

		out[i] = static_cast<int16_t>((left_channel + right_channel) / 2);
		out[i] *= I2S_GAIN;
	}

	return 0;
}

bool LiveMicInput::Start()
{
	if (!device_is_ready(i2s_mic)) {
		LOG_ERR("i2s_mic is not ready");
		return false;
	}

	const struct i2s_config config = {
		.word_size = I2S_WORD_SIZE,
		.channels = I2S_CHANNELS,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_FRAME_CLK_MASTER | I2S_OPT_BIT_CLK_MASTER,
		.frame_clk_freq = static_cast<uint32_t>(I2S_SAMPLE_RATE),
		.mem_slab = &mem_slab,
		.block_size = I2S_BUFFER_SIZE,
		.timeout = SYS_FOREVER_MS,
	};

	int rc = i2s_configure(i2s_mic, I2S_DIR_RX, &config);

	if (rc < 0) {
		LOG_ERR("i2s_configure failed: %i", rc);
		return false;
	}

	rc = i2s_trigger(i2s_mic, I2S_DIR_RX, I2S_TRIGGER_START);
	if (rc != 0) {
		LOG_ERR("I2S_TRIGGER_START failed: %i", rc);
		return false;
	}

	return true;
}

bool LiveMicInput::Stop()
{
	int rc = i2s_trigger(i2s_mic, I2S_DIR_RX, I2S_TRIGGER_DROP);
	if (rc != 0) {
		LOG_ERR("I2S_TRIGGER_DROP failed: %i", rc);
		return false;
	}

	return true;
}

bool LiveMicInput::GetInputData(void *buffer)
{
	void *slab = NULL;
	size_t slab_size = 0;
	size_t offset = 0;

	int16_t *output_buffer = static_cast<int16_t *>(buffer);

	while (OutputSize > offset) {
		int rc = i2s_read(i2s_mic, &slab, &slab_size);
		if (rc != 0) {
			LOG_ERR("i2s read failed: %i", rc);
			return false;
		}

		int stereo_samples = slab_size / I2S_SAMPLE_SIZE;

		rc = i2s_mix_mono_output(static_cast<int16_t *>(slab), stereo_samples,
					 output_buffer + offset / sizeof(int16_t),
					 OutputSize - offset);

		if (rc < 0) {
			k_mem_slab_free(&mem_slab, slab);
			LOG_ERR("i2s_mix_mono_output failed: %i", rc);
			return false;
		}

		k_mem_slab_free(&mem_slab, slab);
		offset += (stereo_samples / 2) * sizeof(int16_t); // In bytes
	}

	return true;
}
