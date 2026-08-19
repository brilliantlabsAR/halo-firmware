/*
 * Copyright (C) 2024 Alif Semiconductor.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/audio/dmic.h>
#include <zephyr/drivers/pdm/pdm_alif.h>
#include <string.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(PDM, LOG_LEVEL_INF);

#define PDM_NODE	DT_ALIAS(pdm_audio)

#define CHANNEL_4  4
#define CHANNEL_5  5

/* PDM Channel 4 configurations */
#define CH4_PHASE			0x0000001F
#define CH4_GAIN			0x0000000D
#define CH4_PEAK_DETECT_TH		0x00060002
#define CH4_PEAK_DETECT_ITV		0x0004002D

/* PDM Channel 5 configurations */
#define CH5_PHASE			0x00000003
#define CH5_GAIN			0x00000013
#define CH5_PEAK_DETECT_TH		0x00060002
#define CH5_PEAK_DETECT_ITV		0x00020027

#define SAMPLE_BIT_WIDTH		16
#define TIMEOUT				5000

struct PDM_CH_CONFIG pdm_coef_reg;

uint32_t ch4_fir[18] = {0x00000001, 0x00000003, 0x00000003, 0x000007F4,
		0x00000004, 0x000007ED, 0x000007F5, 0x000007F4, 0x000007D3,
		0x000007FE, 0x000007BC, 0x000007E5, 0x000007D9, 0x00000793,
		0x00000029, 0x0000072C, 0x00000072, 0x000002FD};

/* Channel 5 FIR coefficient */
uint32_t ch5_fir[18] = {0x00000000, 0x000007FF, 0x00000000, 0x00000004,
		0x00000004, 0x000007FC, 0x00000000, 0x000007FB, 0x000007E4,
		0x00000000, 0x0000002B, 0x00000009, 0x00000016, 0x00000049,
		0x00000793, 0x000006F8, 0x00000045, 0x00000178};

/* The number of channels tested - changes with the test case */
#define NUM_CHANNELS	2
/*
 * Driver will allocate blocks from this slab to receive audio data into them.
 * Application, after getting a given block from the driver and processing its
 * data, needs to free that block.
 */
#define PCMJ_BLOCK_SIZE		30000

/* Number of blocks in the slab */
#define MEM_SLAB_NUM_BLOCKS		2

/* size of buffer where the whole data is stored */
#define DATA_SIZE	(PCMJ_BLOCK_SIZE * MEM_SLAB_NUM_BLOCKS)

static uint8_t pcmj_data[DATA_SIZE];

K_MEM_SLAB_DEFINE(mem_slab, PCMJ_BLOCK_SIZE, MEM_SLAB_NUM_BLOCKS, 4);

/*
 * The list of channels to test
 * The number of channels should match the PDM_CHANNELS
 */
#define PDM_CHANNELS	PDM_MASK_CHANNEL_4 | PDM_MASK_CHANNEL_5

void pdm_ch_config(void)
{
	const struct device *pcmj_device = DEVICE_DT_GET(PDM_NODE);

	pdm_set_ch_phase(pcmj_device, CHANNEL_4, CH4_PHASE);

	pdm_set_ch_gain(pcmj_device, CHANNEL_4, CH4_GAIN);

	pdm_set_peak_detect_th(pcmj_device, CHANNEL_4, CH4_PEAK_DETECT_TH);

	pdm_set_peak_detect_itv(pcmj_device, CHANNEL_4, CH4_PEAK_DETECT_ITV);

	pdm_coef_reg.ch_num  = 4;

	/* Channel 4 fir coefficient */
	memcpy(pdm_coef_reg.ch_fir_coef, ch4_fir, sizeof(pdm_coef_reg.ch_fir_coef));

	/* Channel IIR Filter Coefficient */
	pdm_coef_reg.ch_iir_coef	= 0x00000004;

	pdm_channel_config(pcmj_device, &pdm_coef_reg);

	pdm_set_ch_gain(pcmj_device, CHANNEL_5, CH5_GAIN);

	pdm_set_ch_phase(pcmj_device, CHANNEL_5, CH5_PHASE);

	pdm_set_peak_detect_th(pcmj_device, CHANNEL_5, CH5_PEAK_DETECT_TH);

	pdm_set_peak_detect_itv(pcmj_device, CHANNEL_5, CH5_PEAK_DETECT_ITV);

	pdm_coef_reg.ch_num  = 5;

	/* Channel 5 fir coefficient */
	memcpy(pdm_coef_reg.ch_fir_coef, ch5_fir, sizeof(pdm_coef_reg.ch_fir_coef));

	/* Channel IIR Filter Coefficient */
	pdm_coef_reg.ch_iir_coef	= 0x00000004;

	pdm_channel_config(pcmj_device, &pdm_coef_reg);

	pdm_mode(pcmj_device, PDM_MODE_STANDARD_VOICE_512_CLK_FRQ);
}

static int config_channel(const struct device *dmic_dev,
				struct dmic_cfg *cfg, uint8_t block_count)
{
	int rc;
	int k = 0;
	int i = 0;
	uint32_t data;

	if (dmic_dev == NULL || cfg == NULL) {
		LOG_ERR("%s: input invalid\n", __func__);
		return -1;
	}

	dmic_configure(dmic_dev, cfg);

	pdm_ch_config();

	printk("Start Speaking or Play some Audio!\n");

	rc = dmic_trigger(dmic_dev, DMIC_TRIGGER_START);
	if (rc < 0) {
		LOG_ERR("dmic_trigger error\n");
		return rc;
	}

	void *buffer;

	k = 0;

	for (i = 0; i < block_count; ++i) {
		rc = dmic_read(dmic_dev, 0, &buffer, &data, TIMEOUT);
		if (rc < 0) {
			dmic_trigger(dmic_dev, DMIC_TRIGGER_STOP);
			return rc;
		}

		/* copy the data from the buffer to the pcmj data */
		if (k + data <= DATA_SIZE) {
			memcpy(pcmj_data + k, buffer, data);
			k += data;
		}

		k_mem_slab_free(&mem_slab, buffer);
	}

	printk("Stop recording\n");
	LOG_INF("PCM samples will be stored in %p address and size of "
	"buffer is %d\n", (void *)pcmj_data, sizeof(pcmj_data));
	LOG_INF("Block freed at address: %p\n", (void *)buffer);

	rc = dmic_trigger(dmic_dev, DMIC_TRIGGER_STOP);
	return rc;
}

void print_data(void)
{
	int32_t i;
	/*
	 * print the pointer where the data is stored and first 80 bytes of data
	 */
	LOG_INF("pcm data : %p\n", (void *)pcmj_data);

	for (i = 0; i < 80; i += 8) {
		LOG_INF(" %x %x %x %x %x %x %x %x\n", pcmj_data[i], pcmj_data[i+1],
		pcmj_data[i+2], pcmj_data[i+3], pcmj_data[i+4], pcmj_data[i+5],
		pcmj_data[i+6], pcmj_data[i+7]);
	} k_msleep(500);
}

void set_config(struct dmic_cfg *cfg, struct pcm_stream_cfg *stream)
{
	uint32_t channel_map = 0;

	stream->pcm_width = SAMPLE_BIT_WIDTH;
	cfg->streams = stream;
	cfg->streams[0].mem_slab = &mem_slab;
	cfg->channel.req_num_streams = 1;
	cfg->channel.req_num_chan = NUM_CHANNELS;
	cfg->streams[0].block_size = PCMJ_BLOCK_SIZE;

	channel_map = PDM_CHANNELS;

	cfg->channel.req_chan_map_lo = channel_map;

	LOG_INF("memslab: %p\n", cfg->streams[0].mem_slab);
	LOG_INF("channel_map %x block size: %x\n", channel_map, PCMJ_BLOCK_SIZE);
}

void init_pdm(void)
{
	const struct device *pcmj_device;
	struct dmic_cfg cfg;
	struct pcm_stream_cfg stream;

	LOG_INF("PDM init okay\n");

	pcmj_device = DEVICE_DT_GET(PDM_NODE);

	if (!pcmj_device) {
		LOG_ERR("pcmj_device not found\n");
		return;
	}

	if (!device_is_ready(pcmj_device)) {
		LOG_ERR("device not ready\n");
		return;
	}

	set_config(&cfg, &stream);

	config_channel(pcmj_device, &cfg, MEM_SLAB_NUM_BLOCKS);

	print_data();
}

int main(void)
{
	init_pdm();

	return 0;
}
