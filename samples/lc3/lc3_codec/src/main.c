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
#include <math.h>
#include <stdlib.h>
#include "alif_lc3.h"

LOG_MODULE_REGISTER(main);

#define LC3_APP_SAMPLE_RATE	24000
#define LC3_APP_BITRATE		48000
#define MAX_BYTE_COUNT		400
#define LC3_APP_FRAME_DURATION	FRAME_DURATION_10_MS
#define AUDIO_FRAME_SAMPLES	240
#define ENCODED_DATA_LEN	60
#define SIGNAL_AMPLITUDE	30000.0f
#define SIGNAL_FREQUENCY	500.0f
/* M_PI is not defined in math.h for some reason */
#define PI                     3.14159265358979323846f

static void visualise_audio_data(int16_t *audio, size_t len, size_t downsample)
{
	const uint32_t rows = 10;
	const uint32_t columns = len / downsample;

	const int32_t range = INT16_MAX - INT16_MIN;
	const int16_t bin_size = range / rows;

	for (uint32_t i = 0; i < rows; i++) {
		const int32_t min = INT16_MIN + i * bin_size;
		const int32_t max = INT16_MIN + (i + 1) * bin_size;

		for (uint32_t j = 0; j < columns; j++) {
			const int16_t sample = audio[j * downsample];

			if (sample >= min && sample < max) {
				printk("#");
			} else {
				printk(" ");
			}
		}

		printk("\n");
	}
}

int main(void)
{
	LOG_INF("LC3 codec demo starting");

	/* Initialise the LC3 ROM. This sets up any RAM areas used by the LC3 codec image in ROM,
	 * and applies a patch if one is supplied.
	 */
	int ret = alif_lc3_init();

	__ASSERT(ret == 0, "Failed to initialise LC3 ROM, err %d", ret);

	/* Create and initialise an LC3 config struct. This can be shared by encoder and decoder
	 * instances using the same sample rate and frame duration.
	 */
	static lc3_cfg_t lc3_config;

	ret = lc3_api_configure(&lc3_config, LC3_APP_SAMPLE_RATE, LC3_APP_FRAME_DURATION);
	__ASSERT(ret == 0, "Failed to configure LC3 codec, err %d", ret);

	/* Allocate memory to be used by encoder and decoder */
	int32_t *p_encoder_scratch = malloc(lc3_api_encoder_scratch_size(&lc3_config));

	__ASSERT(p_encoder_scratch, "Failed to allocate encoder scratch memory");

	int32_t *p_decoder_scratch = malloc(lc3_api_decoder_scratch_size(&lc3_config));

	__ASSERT(p_decoder_scratch, "Failed to allocate decoder scratch memory");

	int32_t *p_decoder_status = malloc(lc3_api_decoder_status_size(&lc3_config));

	__ASSERT(p_decoder_status, "Failed to allocate decoder status memory");

	/* Initialise encoder and decoder */
	static lc3_encoder_t lc3_encoder;

	ret = lc3_api_initialise_encoder(&lc3_config, &lc3_encoder);
	__ASSERT(ret == 0, "Failed to initialise LC3 encoder");

	static lc3_decoder_t lc3_decoder;

	ret = lc3_api_initialise_decoder(&lc3_config, &lc3_decoder, p_decoder_status);
	__ASSERT(ret == 0, "Failed to initialise LC3 decoder");

	LOG_INF("LC3 encoder and decoder initialised");

	/* Create some input audio data. In this case it's just a single frequency tone (sine wave)
	 */
	static int16_t input_audio[AUDIO_FRAME_SAMPLES];

	for (uint32_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
		input_audio[i] = SIGNAL_AMPLITUDE *
				 (float)sin((2.0f * PI * SIGNAL_FREQUENCY * i)
				 / LC3_APP_SAMPLE_RATE);
	}

	LOG_INF("Input audio data:");
	visualise_audio_data(input_audio, AUDIO_FRAME_SAMPLES, 2);

	uint16_t byte_count;
	uint8_t bytes[MAX_BYTE_COUNT];

	byte_count = lc3_api_get_byte_count(LC3_APP_BITRATE, LC3_APP_SAMPLE_RATE,
		LC3_APP_FRAME_DURATION);

	if (byte_count > MAX_BYTE_COUNT) {
		LOG_ERR("byte_count exceeded MAX_BYTE_COUNT!");
		return -1;
	}

	/* Encode the data */
	ret = lc3_api_encode_frame(&lc3_config, &lc3_encoder, input_audio, bytes,
				   byte_count, p_encoder_scratch);
	__ASSERT(ret == 0, "Failed to encode frame with err %d", ret);

	LOG_INF("Frame endoded");

	/* Decode the data */
	static int16_t output_audio[AUDIO_FRAME_SAMPLES];
	uint8_t bec_detect = 0;

	ret = lc3_api_decode_frame(&lc3_config, &lc3_decoder, bytes, byte_count, 0,
				   &bec_detect, output_audio, p_decoder_scratch);
	__ASSERT(ret == 0, "Failed to decode frame with err %d", ret);
	__ASSERT(bec_detect == 0, "LC3 decoder detected error with input data");

	LOG_INF("Frame decoded");

	/* Visualise the output so it can be compared with the input. Note: The LC3 codec uses
	 * context from the previous frames during decoding of the current frame. So results will
	 * improve once a few consective frames have been encoded and decoded.
	 */
	LOG_INF("Output audio data:");
	visualise_audio_data(output_audio, AUDIO_FRAME_SAMPLES, 2);

	while (1) {
		k_sleep(K_SECONDS(5));
	}
}
