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
#include <drivers/i2s_sync.h>

LOG_MODULE_REGISTER(main);

/* At 48 kHz, stereo, 16-bit, this gives 960 / 2 / 2 = 240 samples per channel per block, which is 5
 * ms of audio
 */
#define I2S_BLOCK_SIZE_BYTES 960
#define I2S_BLOCK_COUNT      4

#define CODEC_NODE DT_ALIAS(audio_codec)
static const struct device *codec_dev = DEVICE_DT_GET(CODEC_NODE);

#define I2S_NODE DT_ALIAS(i2s_bus)
static const struct device *i2s_dev = DEVICE_DT_GET(I2S_NODE);

static volatile uint32_t tx_ctr;
static volatile uint32_t rx_ctr;
static volatile uint32_t err_ctr;
static void *rx_block;
static void *tx_block;
static struct k_mem_slab i2s_mem;
static uint8_t __aligned(4) i2s_mem_buffer[I2S_BLOCK_SIZE_BYTES * I2S_BLOCK_COUNT];

K_MSGQ_DEFINE(i2s_msgq, sizeof(void *), I2S_BLOCK_COUNT, 1);

static void finish_rx(void)
{
	if (rx_block == NULL) {
		LOG_ERR("No RX block to finish");
		return;
	}

	/* Push completed RX block to queue so it can be sent back out */
	int ret = k_msgq_put(&i2s_msgq, &rx_block, K_NO_WAIT);

	if (ret) {
		LOG_ERR("MSGQ put failed, err %d", ret);
	}
}

static void recv_next_block(const struct device *dev)
{
	int ret = k_mem_slab_alloc(&i2s_mem, &rx_block, K_NO_WAIT);

	if (ret) {
		LOG_ERR("Could not allocate RX block");
		return;
	}

	i2s_sync_recv(dev, rx_block, I2S_BLOCK_SIZE_BYTES);
}

static void finish_tx(void)
{
	if (tx_block == NULL) {
		LOG_ERR("No TX block to finish");
		return;
	}

	/* Free the completed TX block so it is available for RX */
	k_mem_slab_free(&i2s_mem, tx_block);
}

static void send_next_block(const struct device *dev)
{
	int ret = k_msgq_get(&i2s_msgq, &tx_block, K_NO_WAIT);

	if (ret) {
		LOG_ERR("MSGQ get failed, err %d", ret);
		return;
	}

	i2s_sync_send(dev, tx_block, I2S_BLOCK_SIZE_BYTES);
}

static void on_i2s_rx_complete(const struct device *dev, enum i2s_sync_status status)
{
	finish_rx();
	recv_next_block(dev);
	rx_ctr++;

	if (status != I2S_SYNC_STATUS_OK) {
		err_ctr++;
	}
}

static void on_i2s_tx_complete(const struct device *dev, enum i2s_sync_status status)
{
	finish_tx();
	send_next_block(dev);
	tx_ctr++;

	if (status != I2S_SYNC_STATUS_OK) {
		err_ctr++;
	}
}

int main(void)
{
	LOG_INF("I2S sync demo starting");

	if (!device_is_ready(codec_dev)) {
		LOG_ERR("WM8904 codec is not ready");
		return -1;
	}

	if (!device_is_ready(i2s_dev)) {
		LOG_ERR("I2S driver is not ready");
		return -1;
	}

	i2s_sync_register_cb(i2s_dev, I2S_DIR_RX, on_i2s_rx_complete);
	i2s_sync_register_cb(i2s_dev, I2S_DIR_TX, on_i2s_tx_complete);

	while (1) {
		/* Clear msgq and reset mem slab ready to start */
		k_msgq_purge(&i2s_msgq);
		k_mem_slab_init(&i2s_mem, i2s_mem_buffer, I2S_BLOCK_SIZE_BYTES, I2S_BLOCK_COUNT);

		/* Start receiving data on I2S interface */
		recv_next_block(i2s_dev);

		/* Wait for first received block to be posted to MSGQ before starting TX */
		int ret = k_msgq_get(&i2s_msgq, &tx_block, K_FOREVER);

		__ASSERT(ret == 0, "Failed to get block from MSGQ");

		/* Add a delay of approximately half a block before starting TX to ensure that there
		 * is always enough data to TX in the queue.
		 */
		k_msleep(2);

		i2s_sync_send(i2s_dev, tx_block, I2S_BLOCK_SIZE_BYTES);

		for (int i = 0; i < 6; i++) {
			k_sleep(K_SECONDS(5));
			LOG_INF("Blocks received %u, sent %u, errors %u", rx_ctr, tx_ctr, err_ctr);
		}

		/* Every 30 seconds, stop I2S for 5 seconds to demonstrate disable API */
		i2s_sync_disable(i2s_dev, I2S_DIR_BOTH);
		LOG_INF("Stopping I2S for 5 seconds");
		k_sleep(K_SECONDS(5));
	}
}
