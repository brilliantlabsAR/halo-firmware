/*
 * Copyright (C) 2024 Alif Semiconductor.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include "temperature.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ALIF_ADC, LOG_LEVEL_INF);

static uint32_t buffer[8];

#define ADC_CHANNEL_0				(0x00)
#define ADC_CHANNEL_1				(0x01)
#define ADC_CHANNEL_2				(0x02)
#define ADC_CHANNEL_3				(0x03)
#define ADC_CHANNEL_4				(0x04)
#define ADC_CHANNEL_5				(0x05)
#define ADC_CHANNEL_6				(0x06)
#define ADC_CHANNEL_7				(0x07)
#define ADC_CHANNEL_8				(0x08)

/****ADC MASK CHANNEL****/
#define ADC_UNMASK_CHANNEL_0			(1 << 0)
#define ADC_UNMASK_CHANNEL_1			(1 << 1)
#define ADC_UNMASK_CHANNEL_2			(1 << 2)
#define ADC_UNMASK_CHANNEL_3			(1 << 3)
#define ADC_UNMASK_CHANNEL_4			(1 << 4)
#define ADC_UNMASK_CHANNEL_5			(1 << 5)
#define ADC_UNMASK_CHANNEL_6			(1 << 6)
#define ADC_UNMASK_CHANNEL_7			(1 << 7)
#define ADC_UNMASK_CHANNEL_8			(1 << 8)

#define ADC_COMPARATOR_THRESHOLD_ABOVE_A	(1 << 0)
#define ADC_COMPARATOR_THRESHOLD_ABOVE_B	(1 << 1)
#define ADC_COMPARATOR_THRESHOLD_BELOW_A	(1 << 2)
#define ADC_COMPARATOR_THRESHOLD_BELOW_B	(1 << 3)
#define ADC_COMPARATOR_THRESHOLD_BETWEEN_A_B	(1 << 4)
#define ADC_COMPARATOR_THRESHOLD_OUTSIDE_A_B	(1 << 5)

#define TEMPERATURE_SENSOR			ADC_CHANNEL_6
#define MAX_NUM_THRESHOLD			(6)

static uint32_t m_samplings_done;
static uint8_t comparator;

uint32_t comp_value[MAX_NUM_THRESHOLD] = {0};

enum adc_action adc_call_back(const struct device *dev,
				const struct adc_sequence *sequence,
				uint16_t sampling_index)
{

	if (comparator & ADC_COMPARATOR_THRESHOLD_ABOVE_A) {
		comp_value[0] += 1;
	}
	if (comparator & ADC_COMPARATOR_THRESHOLD_ABOVE_B) {
		comp_value[1] += 1;
	}
	if (comparator & ADC_COMPARATOR_THRESHOLD_BELOW_A) {
		comp_value[2] += 1;
	}
	if (comparator & ADC_COMPARATOR_THRESHOLD_BELOW_B) {
		comp_value[3] += 1;
	}
	if (comparator & ADC_COMPARATOR_THRESHOLD_BETWEEN_A_B) {
		comp_value[4] += 1;
	}
	if (comparator & ADC_COMPARATOR_THRESHOLD_OUTSIDE_A_B) {
		comp_value[5] += 1;
	}

	++m_samplings_done;

	if (m_samplings_done < 2) {
		return ADC_ACTION_REPEAT;
	} else {
		return ADC_ACTION_FINISH;
	}
}

int main(void)
{
	float temp;
	int ret;

	struct adc_sequence_options adc_seq_options = {
		.callback	= adc_call_back,
		.user_data	= (uint8_t *)&comparator,
	};

	const struct device *adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc0));

	if (!device_is_ready(adc_dev)) {
		LOG_ERR("adc device not ready");
		return -1;
	}

	struct adc_channel_cfg channel_cfg = {
		.differential = 0,
		.channel_id   = ADC_CHANNEL_6,
	};


	struct adc_sequence sequence = {
		.options = &adc_seq_options,
		.buffer  = (void *)buffer,
		.buffer_size = sizeof(buffer),
		.channels = ADC_UNMASK_CHANNEL_6,
	};

	/* Set the channel */
	ret = adc_channel_setup(adc_dev, &channel_cfg);
	if (ret) {
		LOG_ERR("Unable set up channel");
		return -1;
	}

	/* Start reading the samples */
	ret = adc_read(adc_dev, &sequence);
	if (ret) {
		LOG_ERR("Error: ADC_read failed");
		return -1;
	}


	LOG_INF("Allocated memory buffer Address is 0x%X", (uint32_t)buffer);

	if (channel_cfg.channel_id == ADC_CHANNEL_6) {
		temp = (float)get_temperature(buffer[TEMPERATURE_SENSOR]);
		if (temp == -1) {
			LOG_ERR(" Error: Temperature is outside range");
		} else {
			LOG_INF("Current temp %0.1f C", (double)temp);
		}
	}

	LOG_INF("ADC sampling Done");

	return 0;
}
