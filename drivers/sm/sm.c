/**
 * @file sm.c
 * @brief Ship Mode GPIO Driver
 *
 * Copyright (C) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT sm_gpio

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/sm/sm.h>

LOG_MODULE_REGISTER(shipmode, CONFIG_SHIPMODE_LOG_LEVEL);

struct sm_config {
	struct gpio_dt_spec gpio;
};

int shutdown(const struct device *dev)
{
	const struct sm_config *cfg = dev->config;
	int ret;

	LOG_WRN("Shutting down - entering ship mode");

	ret = gpio_pin_set_dt(&cfg->gpio, 1);
	if (ret < 0) {
		LOG_ERR("Failed to shutdown: %d", ret);
		return ret;
	}

	return 0;
}

static int sm_init(const struct device *dev)
{
	const struct sm_config *cfg = dev->config;
	int ret;

	if (!gpio_is_ready_dt(&cfg->gpio)) {
		LOG_ERR("GPIO device not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&cfg->gpio, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure GPIO: %d", ret);
		return ret;
	}
	
	return 0;
}

#define SM_DEVICE_DEFINE(inst)                                            \
	static const struct sm_config sm_config_##inst = {                \
		.gpio = GPIO_DT_SPEC_INST_GET(inst, gpios),               \
	};                                                                \
	DEVICE_DT_INST_DEFINE(inst, sm_init, NULL, NULL,                  \
			      &sm_config_##inst, POST_KERNEL,             \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL);

DT_INST_FOREACH_STATUS_OKAY(SM_DEVICE_DEFINE)
