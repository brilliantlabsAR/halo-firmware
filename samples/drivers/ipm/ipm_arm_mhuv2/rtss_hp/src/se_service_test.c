/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 *
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>
#include <errno.h>
#include <string.h>
#include <se_service.h>

#define BUF_SIZE                   8
#define PRIORITY                   7
#define STACK_SIZE                 1024
#define VERSION_RESPONSE_LENGTH    80
#define DEV_DATA_BUFFER_SIZE       33

uint8_t buffer[BUF_SIZE];
uint8_t revision[VERSION_RESPONSE_LENGTH];
uint32_t dev_part_num;
get_device_revision_data_t dev_data;
static uint8_t dev_data_buffer[DEV_DATA_BUFFER_SIZE] = {0};

int thread0_start(void)
{
	int ret = -1;

	printk("!!!Read board config using SE Services!!!\n");

	ret = se_service_get_rnd_num(buffer, BUF_SIZE);
	if (ret) {
		printk("fetch_rnd_num failed with %d\n", ret);
		return ret;
	}
	ret = se_service_get_device_part_number(&dev_part_num);
	if (ret) {
		printk("fetch_device_part_number failed with %d\n", ret);
		return ret;
	}
	printk("Device part number is %d\n", dev_part_num);
	return 0;
}

/**
 * @brief format_contents - converts numbers in src into
 * characters for displaying.
 * parameters,
 * dst - placeholder containing numbers converted to characters.
 * src - placeholder containing numbers to be converted.
 * bytes - length of numbers to be converted.
 */
void format_contents(uint8_t *dst, const uint8_t *src, int bytes)
{
	uint8_t digit_len = 0;
	int i = 0, pos = 0;

	for (i = 0; i < bytes ; ++i) {
		/* pass digit length as 2 as two charactes need to */
		/* be printed if the number is morethan 0xF */
		if (src[i] > 0xF)
			digit_len = 2;
		/* one character is printed */
		else
			digit_len = 1;

		/* extra 1 for '\0' */
		snprintk(&dst[pos], digit_len + 1, "%x", src[i]);
		pos += digit_len;
	}
}
int thread1_start(void)
{
	int ret = -1;

	ret = se_service_get_rnd_num(buffer, BUF_SIZE);
	if (ret) {
		printk("fetch_rnd_num failed with %d\n", ret);
		return ret;
	}
	ret = se_service_get_se_revision(revision);
	if (ret) {
		printk("fetch_se_revision failed with %d\n", ret);
		return ret;
	}
	printk("Revision is %s\n", revision);

	ret = se_service_system_get_device_data(&dev_data);
	if (ret) {
		printk("failed to get device data (err = %d)\n", ret);
		return ret;
	}

	printk("Revision ID = %d (0x%x)\n", dev_data.revision_id,
		dev_data.revision_id);

	printk("Alif PN = %s\n", dev_data.ALIF_PN);

	format_contents((uint8_t *)&dev_data_buffer[0],
			(uint8_t *)&dev_data.SerialN[0],
			sizeof(dev_data.SerialN));
	printk("Serial Number = %s\n", dev_data_buffer);

	format_contents((uint8_t *)&dev_data_buffer[0],
			(uint8_t *)&dev_data.HBK0[0],
			sizeof(dev_data.HBK0));
	printk("HBK0 = %s\n", dev_data_buffer);

	format_contents((uint8_t *)&dev_data_buffer[0],
			(uint8_t *)&dev_data.DCU[0],
			sizeof(dev_data.DCU));
	printk("DCU settings = %s\n", dev_data_buffer);

	format_contents((uint8_t *)&dev_data_buffer[0],
			(uint8_t *)&dev_data.config[0],
			sizeof(dev_data.config));
	printk("config = %s\n", dev_data_buffer);

	format_contents((uint8_t *)&dev_data_buffer[0],
			(uint8_t *)&dev_data.HBK1[0],
			sizeof(dev_data.HBK1));
	printk("HBK1 = %s\n", dev_data_buffer);

	format_contents((uint8_t *)&dev_data_buffer[0],
			(uint8_t *)&dev_data.HBK_FW[0],
			sizeof(dev_data.HBK_FW));
	printk("HBK_FW = %s\n", dev_data_buffer);

	format_contents((uint8_t *)&dev_data_buffer[0],
			(uint8_t *)&dev_data.MfgData[0],
			sizeof(dev_data.MfgData));
	printk("MfgData = %s\n", dev_data_buffer);

	printk("LCS = %d (0x%x)\n", dev_data.LCS, dev_data.LCS);

	printk("!!!Board config read successfully!!!\n");
	return 0;
}


K_THREAD_DEFINE(thread0, STACK_SIZE, thread0_start,
		NULL, NULL, NULL, PRIORITY, 0, 0);
K_THREAD_DEFINE(thread1, STACK_SIZE, thread1_start,
		NULL, NULL, NULL, PRIORITY, 0, 0);

int main(void)
{
	k_thread_start(thread0);
	k_thread_start(thread1);
	return 0;
}
