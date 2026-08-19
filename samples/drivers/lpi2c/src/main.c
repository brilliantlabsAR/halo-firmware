/*
 * Copyright (C) 2024 Alif Semiconductor.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ALIF_LPI2C, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <string.h>
#include <zephyr/drivers/i2c.h>
#include <soc.h>

/* i2c_master and lpi2c_slave aliases are defined in
 * overlay files to use different SPI instance if needed.
 */

#define SLV_I2C_ADDR	0x40
#define I2C_MASTER	DT_ALIAS(master_i2c)
#define I2C_SLAVE	DT_ALIAS(slave_i2c)

/* size of stack area used by each thread */
#define STACKSIZE 1024

/* scheduling priority used by each thread */
#define MASTER_PRIORITY 5
#define SLAVE_PRIORITY 6

K_THREAD_STACK_DEFINE(MasterT_stack, STACKSIZE);
static struct k_thread MasterT_data;

K_THREAD_STACK_DEFINE(SlaveT_stack, STACKSIZE);
static struct k_thread SlaveT_data;

uint8_t mst_tx_data[20] = {"!!Text From Master!!"};
uint8_t mst_rx_data[19];
uint8_t slv_tx_data[19] = {"!!Text From Slave!!"};
uint8_t slv_rx_data[20];

/* master transmit and slave receive */
#define MST_BYTE_TO_TRANSMIT            20

/* slave transmit and master receive */
#define SLV_BYTE_TO_TRANSMIT            19

uint32_t mst_rx_cnt;
uint32_t slv_rx_cnt;
struct k_event mst_event;
struct k_event slv_event;

#define MST_EVENT  0x1
#define SLV_EVENT  0x2

static void i2c_master(void *p1, void *p2, void *p3)
{
	int ret;
	uint32_t  events;
	struct i2c_msg msgs[2];

	const struct device *const i2c_master_dev = DEVICE_DT_GET(I2C_MASTER);

	ret = device_is_ready(i2c_master_dev);
	if (!ret) {
		printk("i2c: Master Device is not ready.\n");
	}

	/* initialize event */
	k_event_init(&mst_event);

	/* Setup i2c messages
	 * Data to be written, and STOP after this.
	 */
	msgs[0].buf = mst_tx_data;
	msgs[0].len = MST_BYTE_TO_TRANSMIT;
	msgs[0].flags = I2C_MSG_WRITE | I2C_MSG_STOP;

	msgs[1].buf = mst_rx_data;
	msgs[1].len = 1U;
	msgs[1].flags = I2C_MSG_READ | I2C_MSG_STOP;


	LOG_INF("Start Master trasmit and Slave receive");

	/* Writing data from Master to Slave */
	ret = i2c_transfer(i2c_master_dev, &msgs[0], 1, SLV_I2C_ADDR);
	if (ret) {
		LOG_ERR("error on transfer with : %d", ret);
	}

	/* waiting for event to occur */
	events = k_event_wait(&mst_event, MST_EVENT, false, K_FOREVER);

	/* Delay */
	k_busy_wait(1000);

	for (int i = 0; i < SLV_BYTE_TO_TRANSMIT; i++) {

		msgs[1].buf = &mst_rx_data[i];
		ret = i2c_transfer(i2c_master_dev, &msgs[1], 1, SLV_I2C_ADDR);
		if (ret) {
			LOG_ERR("error on Reception with : %d", ret);
		}
	}

	if (memcmp(mst_rx_data, slv_tx_data, SLV_BYTE_TO_TRANSMIT)) {
		LOG_ERR("Error: Master transmit/slave receive failed");
		LOG_ERR(" ---Stop--- \r\n wait forever >>>");
		while (1) {
			;
		}
	}

	LOG_INF("Slave transmit and Master receive successful");
	LOG_INF("Transfer completed");
}

int i2c_target_write_received_cb(struct i2c_target_config *config, uint8_t val)
{
	slv_rx_data[slv_rx_cnt++] = val;

	if (slv_rx_cnt == MST_BYTE_TO_TRANSMIT) {
		k_event_set(&slv_event, SLV_EVENT);
	}
	return 0;
}

void register_slave_i2c(struct i2c_target_config *cfg)
{
	int ret = 0;

	const struct device *const i2c_slave_dev = DEVICE_DT_GET(DT_NODELABEL(lpi2c));

	if (!device_is_ready(i2c_slave_dev)) {
		LOG_ERR("lpi2c: Slave Device is not ready.");
		return;
	}

	ret = i2c_target_register(i2c_slave_dev, cfg);
}

static void lpi2c_slave(void *p1, void *p2, void *p3)
{
	int ret;
	uint32_t  events;
	struct i2c_msg msgs[1];

	/* initialize event */
	k_event_init(&slv_event);

	msgs[0].buf = slv_tx_data;
	msgs[0].len = SLV_BYTE_TO_TRANSMIT;

	struct i2c_target_callbacks i2c_t_cb = {
		.write_received = &i2c_target_write_received_cb,
	};
	struct i2c_target_config tcfg = {
		.callbacks = &i2c_t_cb
	};
	register_slave_i2c(&tcfg);

	/* waiting for event to occur */
	events = k_event_wait(&slv_event, SLV_EVENT, false, K_FOREVER);

	if (memcmp(slv_rx_data, mst_tx_data, MST_BYTE_TO_TRANSMIT)) {
		LOG_ERR("Error: Master transmit/slave receive failed ");
		LOG_ERR("---Stop--- \r\n wait forever >>>");
		while (1) {
			;
		}
	}

	LOG_INF("Master transmit and slave receive successful");

	k_event_set(&mst_event, MST_EVENT);

	LOG_INF("Start Slave transmit and Master receive");

	const struct device *const i2c_slave_dev = DEVICE_DT_GET(DT_NODELABEL(lpi2c));

	/* Slave transmitting bytes to master */
	ret = i2c_transfer(i2c_slave_dev, &msgs[0], 1, SLV_I2C_ADDR);
	if (ret) {
		LOG_ERR("error on slave transmission with : %d", ret);
	}
}

int main(void)
{
	k_tid_t tids = k_thread_create(&SlaveT_data, SlaveT_stack, STACKSIZE,
			&lpi2c_slave, NULL, NULL, NULL,
			SLAVE_PRIORITY, 0, K_NO_WAIT);
	if (tids == NULL) {
		printk("Error creating Slave Thread\n");
	}

	k_tid_t tidm = k_thread_create(&MasterT_data, MasterT_stack, STACKSIZE,
			&i2c_master, NULL, NULL, NULL,
			MASTER_PRIORITY, 0, K_NO_WAIT);
	if (tidm == NULL) {
		printk("Error creating Master Thread\n");
	}

	k_thread_start(&MasterT_data);
	k_thread_start(&SlaveT_data);

	return 0;
}
