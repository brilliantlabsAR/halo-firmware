/*
 * Copyright (C) 2024 Alif Semiconductor.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/audio/dmic.h>
#include <zephyr/drivers/sensor.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include <t5838.h>

LOG_MODULE_REGISTER(PDM, LOG_LEVEL_INF);

#define PDM_NODE DT_CHOSEN(zephyr_micphone)

#define PDM_SAMPLE_RATE  8000
#define SAMPLE_BIT_WIDTH 16
#define NUM_CHANNELS     2
#define DURATION_MS      5000
#define INTERVAL_MS      100 // 100ms interval
#define PDM_CHANNELS     (BIT(2) | BIT(3))

#define PDM_BLOCK_SIZE      (INTERVAL_MS * PDM_SAMPLE_RATE * SAMPLE_BIT_WIDTH * NUM_CHANNELS / 8 / 1000)
#define DATA_SIZE           (PDM_BLOCK_SIZE * (DURATION_MS / INTERVAL_MS))
#define MEM_SLAB_NUM_BLOCKS (DURATION_MS / INTERVAL_MS)

static uint8_t pdm_data[DATA_SIZE];

// every 100ms allocate a block from the slab
K_MEM_SLAB_DEFINE(mem_slab, PDM_BLOCK_SIZE, MEM_SLAB_NUM_BLOCKS, 4);

K_SEM_DEFINE(mic_sem, 0, 1);
void t5838_cb(const struct device *dev)
{
	LOG_INF("T5838 interrupt triggered");
	const struct device *const pdm_aad_dev = DEVICE_DT_GET(DT_NODELABEL(t5838));
	t5838_aad_wake_clear(pdm_aad_dev);
	k_sem_give(&mic_sem);
}

int main(void)
{

	const struct device *pdm_dev = DEVICE_DT_GET(PDM_NODE);
	const struct device *const pdm_aad_dev = DEVICE_DT_GET(DT_NODELABEL(t5838));
	if (!device_is_ready(pdm_dev)) {
		LOG_ERR("PDM device not ready, aborting test");
		return -ENODEV;
	}

	// struct dmic_cfg cfg;
	// struct pcm_stream_cfg stream;
	// int ret;

	// stream.pcm_width = SAMPLE_BIT_WIDTH;
	// stream.pcm_rate = PDM_SAMPLE_RATE;
	// stream.mem_slab = &mem_slab;
	// stream.block_size = PDM_BLOCK_SIZE;

	// cfg.channel.req_num_streams = 1;
	// cfg.channel.req_num_chan = NUM_CHANNELS;
	// cfg.channel.req_chan_map_lo = PDM_CHANNELS; // PDM channels 2 and 3
	// cfg.channel.req_chan_map_hi = 0;
	// cfg.streams = &stream;

	// int rc = dmic_configure(pdm_dev, &cfg);
	// if (rc < 0) {
	// 	LOG_ERR("Failed to configure PDM: %d", rc);
	// 	return rc;
	// }

	/*AAD A CONFIGURATION */
	struct t5838_aad_a_conf aadcfg = {
		.aad_a_lpf = T5838_AAD_A_LPF_2_0kHz,
		.aad_a_thr = T5838_AAD_A_THR_90dB,
		.silent_period = 1000, // in ms
	};
	t5838_aad_a_mode_set(pdm_aad_dev, &aadcfg);
	t5838_aad_wake_handler_set(pdm_aad_dev, t5838_cb);

	while (1) {

		LOG_INF("Waiting on dmic trigger");
		k_sem_take(&mic_sem, K_FOREVER);
		LOG_INF("DMIC Triggered by AAD");
		// printf("Start Speaking or Play some Audio!\n");
		// rc = dmic_trigger(pdm_dev, DMIC_TRIGGER_START);
		// if (rc < 0) {
		// 	LOG_ERR("Failed to start PDM recording: %d", rc);
		// 	return rc;
		// }
		// uint32_t total_bytes = 0;
		// uint32_t data;
		// for (int i = 0; i < MEM_SLAB_NUM_BLOCKS; ++i) {
		// 	void *buffer;
		// 	printf("Block %d %d\n", i, k_uptime_get_32());
		// 	rc = dmic_read(pdm_dev, 0, &buffer, &data, 5000);
		// 	if (rc < 0) {
		// 		dmic_trigger(pdm_dev, DMIC_TRIGGER_STOP);
		// 		LOG_ERR("dmic_read error\n");

		// 		return rc;
		// 	}

		// 	/* copy the data from the buffer to the pcmj data */
		// 	if (total_bytes + data <= DATA_SIZE) {
		// 		memcpy(pdm_data + total_bytes, buffer, data);
		// 		total_bytes += data;
		// 	}

		// 	k_mem_slab_free(&mem_slab, buffer);
		// }
		// printf("Stop recording\n");
		// rc = dmic_trigger(pdm_dev, DMIC_TRIGGER_STOP);

		// printf("PDM example, record done\n");
		// uint16_t *pdm_data16 = (uint16_t *)pdm_data;
		// for (int i = 0; i < DATA_SIZE / 2; i++) {
		// 	printf("%04x\n", pdm_data16[i]);
		// }
		// printf("PDM example, print done\n");
		// t5838_aad_wake_clear(pdm_aad_dev);
	}

	return 0;
}
