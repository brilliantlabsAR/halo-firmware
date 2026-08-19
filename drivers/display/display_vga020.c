/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT gz_vga020

#include <zephyr/dt-bindings/display/panel.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/policy.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>
#ifdef CONFIG_MIPI_DBI
#include <zephyr/drivers/mipi_dbi.h>
#endif
#ifdef CONFIG_MIPI_DSI
#include <zephyr/drivers/mipi_dsi.h>
#include <zephyr/kernel.h>
#endif

LOG_MODULE_REGISTER(display_vga020, CONFIG_DISPLAY_LOG_LEVEL);

#define VGA020_PHY_WIDTH          660
#define VGA020_PHY_HEIGHT         500

#define VGA020_I2C_RETRY_COUNT    3
#define VGA020_I2C_RETRY_DELAY_MS 5

#define V_TOTAL 365
#define PWM_MIN 0x0000
#define PWM_MAX V_TOTAL

#define VGA020_REG_TEST_PATTERN     0x0001
#define VGA020_REG_SCANNING_MODE    0x0002
#define VGA020_REG_MOVE_CONTROL     0x0200
#define VGA020_REG_MOVE_TICK_L      0x0201
#define VGA020_REG_MOVE_TICK_H      0x0202
#define VGA020_REG_RED_GRADIENT     0x0300
#define VGA020_REG_GREEN_GRADIENT   0x0301
#define VGA020_REG_BLUE_GRADIENT    0x0302
#define VGA020_REG_TOP              0x0400
#define VGA020_REG_BOTTOM           0x0401
#define VGA020_REG_LEFT             0x0402
#define VGA020_REG_RIGHT            0x0403
#define VGA020_REG_CONSTRAST_A_OUT  0x0600
#define VGA020_REG_CONSTRAST_B_OUT  0x0601
#define VGA020_REG_CONSTRAST_A_IN   0x0602
#define VGA020_REG_CONSTRAST_B_IN   0x0603
#define VGA020_REG_BRIGHTNESS       0x8000
#define VGA020_REG_COPY_MODE        0x3001
#define VGA020_REG_TEMP             0x3003
#define VGA020_REG_PWM              0x5800
#define VGA020_REG_EN               0x7F01
#define VGA020_REG_LANE             0x5F00
#define VGA020_REG_VEE_EN           0x7402
#define VGA020_REG_PWM_MODE         0x7602
#define VGA020_REG_DUAL_BYTE_ENABLE 0x3000
#define VGA020_REG_PWM_HIGH         0x5801
#define VGA020_REG_PWM_LOW          0x5800

#define VGA020_SPI_CMD_HSS       0x11
#define VGA020_SPI_CMD_WCB_START 0x2C
#define VGA020_SPI_CMD_WCB_WRITE 0x2E

#define HFP   16
#define HBP   48
#define HSYNC 96
#define VFP   16
#define VBP   26
#define VSYNC 3

/* Display data struct */
struct vga020_data {
	bool is_initialized;
	bool attached;
	uint8_t brightness;
	uint8_t bytes_per_pixel;
	enum display_pixel_format pixel_format;
	enum display_orientation orientation;
	int8_t pan_x;  /* Pan offset X: -50 to +50 */
	int8_t pan_y;  /* Pan offset Y: -50 to +50 */
};

/* Configuration data struct.*/
struct vga020_config {
	struct i2c_dt_spec i2c;
	struct gpio_dt_spec reset_gpio;
	const struct device *mipi_dev;
	const struct device *vdd_reg;
	const struct device *vpos_reg;
	const struct device *veng_reg;
	uint8_t pixel_format;
	uint16_t x_resolution;
	uint16_t y_resolution;
	bool lazy_init;
};

static int vga020_write_reg(const struct device *dev

			    ,
			    uint16_t reg, uint8_t value)
{
	int ret = 0;
	const struct vga020_config *config = dev->config;
	uint8_t buf[3];
	buf[0] = (reg >> 8) & 0xFF;
	buf[1] = reg & 0xFF;
	buf[2] = value;

	/* Write to i2c with retry mechanism */
	for (int retry = 0; retry < VGA020_I2C_RETRY_COUNT; retry++) {
		ret = i2c_write_dt(&config->i2c, buf, 3);
		if (ret == 0) {
			return 0;
		}
		if (retry < VGA020_I2C_RETRY_COUNT - 1) {
			k_msleep(VGA020_I2C_RETRY_DELAY_MS);
		}
	}

	LOG_ERR("vga020_write_reg failed after %d retries: %d", VGA020_I2C_RETRY_COUNT, ret);
	return ret;
}

__maybe_unused static int vga020_read_reg(const struct device *dev, uint16_t reg, uint8_t *value)
{
	int ret = 0;
	const struct vga020_config *config = dev->config;
	uint8_t buf[2];
	buf[0] = (reg >> 8) & 0xFF;
	buf[1] = reg & 0xFF;

	/* Write/read to i2c with retry mechanism */
	for (int retry = 0; retry < VGA020_I2C_RETRY_COUNT; retry++) {
		ret = i2c_write_read_dt(&config->i2c, buf, 2, value, 1);
		if (ret == 0) {
			return 0;
		}
		if (retry < VGA020_I2C_RETRY_COUNT - 1) {
			k_msleep(VGA020_I2C_RETRY_DELAY_MS);
		}
	}
	return ret;
}

/* Forward declaration */
static int vga020_hw_init(const struct device *dev);

static int vga020_display_blanking_on(const struct device *dev)
{
	int ret = 0;
	// Blank the display
	ret = vga020_write_reg(dev, VGA020_REG_VEE_EN, 0x88);
	if (ret != 0) {
		return ret;
	}

	return ret;
}

static int vga020_display_blanking_off(const struct device *dev)
{
	int ret = 0;
	struct vga020_data *data = dev->data;

	/* Hardware must be initialized before blanking off */
	if (!data->is_initialized) {
		LOG_ERR("Cannot unblank: hardware not initialized. Call "
			"pm_device_action_run(RESUME) first");
		return -ENODEV;
	}

	// Unblank the display
	ret = vga020_write_reg(dev, VGA020_REG_VEE_EN, 0x8C);
	if (ret != 0) {
		return ret;
	}

	return ret;
}

static int vga020_write(const struct device *dev, const uint16_t x, const uint16_t y,
			const struct display_buffer_descriptor *desc, const void *buf)
{

	return -ENOTSUP;
}

static int vga020_read(const struct device *dev, const uint16_t x, const uint16_t y,
		       const struct display_buffer_descriptor *desc, void *buf)
{
	return -ENOTSUP;
}

static void *vga020_get_framebuffer(const struct device *dev)
{
	return NULL;
}

static int vga020_set_brightness(const struct device *dev, const uint8_t brightness)
{
	int ret = 0;
	struct vga020_data *data = dev->data;
	uint8_t safe_brightness = brightness;
	if (safe_brightness > 100) {
		LOG_WRN("Brightness %d exceeds max (100). Clamping.", brightness);
		safe_brightness = 100;
	}

	/* 1. Map brightness level [0~100] to PWM [0x00~0x64] (<= V_TOTAL) */
	uint16_t pwm_value = safe_brightness;

	/* 2. Enable dual-byte register update (0x3000 = 0x3F) */
	ret = vga020_write_reg(dev, VGA020_REG_DUAL_BYTE_ENABLE, 0x003F);
	if (ret) {
		LOG_ERR("Failed to enable dual-byte mode: %d", ret);
		return ret;
	}

	/* 3. Write PWM registers */
	ret = vga020_write_reg(dev, VGA020_REG_PWM_LOW, pwm_value & 0xFF);
	if (ret) {
		LOG_ERR("PWM low byte write failed: %d", ret);
		return ret;
	}

	ret = vga020_write_reg(dev, VGA020_REG_PWM_HIGH, (pwm_value >> 8) & 0x03);
	if (ret) {
		LOG_ERR("PWM high byte write failed: %d", ret);
		return ret;
	}

	/* 4. Commit update (0x3000 = 0x7F) */
	ret = vga020_write_reg(dev, VGA020_REG_DUAL_BYTE_ENABLE, 0x007F);
	if (ret) {
		LOG_ERR("Brightness commit failed: %d", ret);
	}

	data->brightness = safe_brightness;

	return ret;
}

static int vga020_set_contrast(const struct device *dev, const uint8_t contrast)
{
	int ret = 0;

	return ret;
}

static void vga020_get_capabilities(const struct device *dev,
				    struct display_capabilities *capabilities)
{
	struct vga020_data *data = dev->data;
	const struct vga020_config *config = dev->config;

	memset(capabilities, 0, sizeof(struct display_capabilities));

	capabilities->supported_pixel_formats = PIXEL_FORMAT_RGB_888;
	capabilities->current_pixel_format = data->pixel_format;
	capabilities->x_resolution = config->x_resolution;
	capabilities->y_resolution = config->y_resolution;
	capabilities->current_orientation = data->orientation;
}

static int vga020_set_pixel_format(const struct device *dev,
				   const enum display_pixel_format pixel_format)
{
	return -ENOTSUP;
}

static int vga020_set_orientation(const struct device *dev,
				  const enum display_orientation orientation)
{
	return -ENOTSUP;
}

static int vga020_init(const struct device *dev)
{
	int ret = 0;
	const struct vga020_config *config = dev->config;
	struct vga020_data *data = dev->data;

	/* Reset state flags on every init (important for sleep/wake cycles) */
	data->is_initialized = false;
	data->attached = false;

	/* Only verify device dependencies, defer hardware init to first use */
	if (!i2c_is_ready_dt(&config->i2c)) {
		LOG_ERR("I2C device not ready");
		return -ENODEV;
	}

#ifdef CONFIG_MIPI_DSI
	if (config->mipi_dev == NULL) {
		return -1;
	}
#endif

	if (config->reset_gpio.port != NULL) {
		if (!device_is_ready(config->reset_gpio.port)) {
			LOG_ERR("Reset GPIO port %s is not ready", config->reset_gpio.port->name);
		}
		gpio_pin_configure_dt(&config->reset_gpio, GPIO_OUTPUT_ACTIVE);
	}

	if (config->vdd_reg && !device_is_ready(config->vdd_reg)) {
		LOG_ERR("VDD regulator not ready");
		return -ENODEV;
	}

	if (config->vpos_reg && !device_is_ready(config->vpos_reg)) {
		LOG_ERR("VPOS regulator not ready");
		return -ENODEV;
	}
	if (config->veng_reg && !device_is_ready(config->veng_reg)) {
		LOG_ERR("VENG regulator not ready");
		return -ENODEV;
	}

	/* Check if lazy initialization is enabled */
	if (config->lazy_init) {
		/* Lazy init mode: defer hardware initialization until first PM resume
		 * This saves ~150ms+ boot time and power consumption */
		data->is_initialized = false;

#ifdef CONFIG_PM_DEVICE
		/* Mark device as suspended since we haven't initialized hardware yet
		 * This tells the PM framework the device is in a low-power state */
		pm_device_init_suspended(dev);
#endif

		LOG_INF("VGA020 init done (lazy-init mode: hardware init deferred, marked as "
			"suspended)");
	} else {
		/* Normal mode: initialize hardware immediately */
		LOG_INF("VGA020 init: initializing hardware immediately (lazy-init disabled)");
		ret = vga020_hw_init(dev);
		if (ret != 0) {
			LOG_ERR("VGA020 hardware init failed: %d", ret);
			return ret;
		}
		LOG_INF("VGA020 init done (hardware initialized)");
	}

	return ret;
}

/* Hardware initialization - called by Lua layer on first use */
static int vga020_hw_init(const struct device *dev)
{
	int ret = 0;
	const struct vga020_config *config = dev->config;
	struct vga020_data *data = dev->data;

	if (data->is_initialized) {
		return 0; /* Already initialized */
	}

	LOG_INF("VGA020 hardware init starting...");

	if (config->reset_gpio.port != NULL) {
		gpio_pin_set_dt(&config->reset_gpio, 1);
	}

	if (config->vdd_reg) {
		ret = regulator_enable(config->vdd_reg);
		k_msleep(10);
	}

	if (config->vpos_reg) {
		ret = regulator_enable(config->vpos_reg);
		k_msleep(10);
	}

	// reset the device after DSI attach
	if (config->reset_gpio.port != NULL) {
		gpio_pin_set_dt(&config->reset_gpio, 0);
		k_msleep(10);
	}

	if (config->veng_reg) {
		ret = regulator_enable(config->veng_reg);
	}

	k_msleep(100);

#ifdef CONFIG_MIPI_DSI
	/* Attach DSI first before resetting and initializing vga020 */
	if (!data->attached) {
		struct mipi_dsi_device mdev;
		mdev.data_lanes = 1;
		mdev.pixfmt = MIPI_DSI_PIXFMT_RGB888;
		mdev.timings.hactive = config->x_resolution;
		mdev.timings.vactive = config->y_resolution;

		mdev.timings.hfp = HFP;
		mdev.timings.hbp = HBP;
		mdev.timings.hsync = HSYNC;
		mdev.timings.vfp = VFP;
		mdev.timings.vbp = VBP;
		mdev.timings.vsync = VSYNC;

		mdev.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_EOT_PACKET |
				  MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_VIDEO_BURST;

		ret = mipi_dsi_attach(config->mipi_dev, 0, &mdev);
		if (ret < 0) {
			LOG_ERR("Could not attach to MIPI-DSI host");
			goto err_rollback;
		}
		data->attached = true;
		k_msleep(30);
	}
#endif

	ret = vga020_write_reg(dev, 0x6C00, 0x00);
	if (ret) {
		LOG_ERR("Not found vga020 device");
		goto err_rollback;
	}

	goto skip_rollback;

err_rollback:
	// close the device - rollback on error
#ifdef CONFIG_MIPI_DSI
	if (data->attached) {
		mipi_dsi_detach(config->mipi_dev, 0, NULL);
		data->attached = false;
	}
#endif
	if (config->vdd_reg) {
		regulator_disable(config->vdd_reg);
	}
	if (config->vpos_reg) {
		regulator_disable(config->vpos_reg);
	}
	if (config->veng_reg) {
		regulator_disable(config->veng_reg);
	}
	if (config->reset_gpio.port != NULL) {
		gpio_pin_set_dt(&config->reset_gpio, 1);
	}

	return ret;

skip_rollback:

	vga020_write_reg(dev, 0x6900, 0x08);
	vga020_write_reg(dev, 0x6901, 0x00);
	vga020_write_reg(dev, 0x6800, 0x00);
	vga020_write_reg(dev, 0x6801, 0x00);
	vga020_write_reg(dev, 0x6802, 0x00);
	vga020_write_reg(dev, 0x6803, 0x00);
	vga020_write_reg(dev, 0x6C00, 0x70);
	vga020_write_reg(dev, 0x6C00, 0x00);
	vga020_write_reg(dev, 0x6900, 0x10);
	vga020_write_reg(dev, 0x6901, 0x00);
	vga020_write_reg(dev, 0x6800, 0x07);
	vga020_write_reg(dev, 0x6801, 0x00);
	vga020_write_reg(dev, 0x6802, 0x00);
	vga020_write_reg(dev, 0x6803, 0x00);
	vga020_write_reg(dev, 0x6C00, 0x70);
	vga020_write_reg(dev, 0x6C00, 0x00);
	vga020_write_reg(dev, 0x6900, 0x00);
	vga020_write_reg(dev, 0x6901, 0x04);
	vga020_write_reg(dev, 0x6800, 0x01);
	vga020_write_reg(dev, 0x6801, 0x00);
	vga020_write_reg(dev, 0x6802, 0x00);
	vga020_write_reg(dev, 0x6803, 0x00);
	vga020_write_reg(dev, 0x6C00, 0x70);
	vga020_write_reg(dev, 0x6C00, 0x00);
	vga020_write_reg(dev, 0x6900, 0x04);
	vga020_write_reg(dev, 0x6901, 0x04);
	vga020_write_reg(dev, 0x6800, 0x00);
	vga020_write_reg(dev, 0x6801, 0x00);
	vga020_write_reg(dev, 0x6802, 0x00);
	vga020_write_reg(dev, 0x6803, 0x00);
	vga020_write_reg(dev, 0x6C00, 0x70);
	vga020_write_reg(dev, 0x6C00, 0x00);
	vga020_write_reg(dev, 0x6900, 0x08);
	vga020_write_reg(dev, 0x6901, 0x04);
	vga020_write_reg(dev, 0x6800, 0x1E);
	vga020_write_reg(dev, 0x6801, 0x00);
	vga020_write_reg(dev, 0x6802, 0x00);
	vga020_write_reg(dev, 0x6803, 0x00);
	vga020_write_reg(dev, 0x6C00, 0x70);
	vga020_write_reg(dev, 0x6C00, 0x00);

	// 1 lane
	vga020_write_reg(dev, 0x5F00, 0x20);
	vga020_write_reg(dev, 0x7F00, 0x0C);
	vga020_write_reg(dev, 0x0102, 0x1F);
	
	// calculate centering offsets
	uint8_t top = (VGA020_PHY_HEIGHT - config->y_resolution) / 2;
	uint8_t bottom = VGA020_PHY_HEIGHT - config->y_resolution - top;
	uint8_t left = (VGA020_PHY_WIDTH - config->x_resolution) / 2;
	uint8_t right = VGA020_PHY_WIDTH - config->x_resolution - left;
	vga020_write_reg(dev, 0x0400, top);
	vga020_write_reg(dev, 0x0401, bottom);
	vga020_write_reg(dev, 0x0402, left);
	vga020_write_reg(dev, 0x0403, right);

	vga020_write_reg(dev, 0x7F01, 0x08);

	vga020_write_reg(dev, VGA020_REG_SCANNING_MODE, 0x03);

	vga020_set_brightness(dev, data->brightness);

#ifndef CONFIG_MIPI_DSI
	vga020_write_reg(dev, 0x0001, 0xC5);
#endif

	data->is_initialized = true;
	LOG_INF("VGA020 hardware initialized");
	return ret;
}

#ifdef CONFIG_PM_DEVICE
__maybe_unused static int vga020_power_off(const struct device *dev)
{
	int ret = 0;
	const struct vga020_config *config = dev->config;
	struct vga020_data *data = dev->data;

#ifdef CONFIG_MIPI_DSI
	if (config->mipi_dev != NULL && data->attached) {
		ret = mipi_dsi_detach(config->mipi_dev, 0, NULL);
		if (ret) {
			LOG_ERR("Could not detach from MIPI-DSI host");
			return ret;
		}
		data->attached = false;
	}
#endif
	// assert reset pin
	if (config->reset_gpio.port != NULL) {
		gpio_pin_set_dt(&config->reset_gpio, 1);
		k_msleep(10);
	}

	if (config->veng_reg) {
		ret = regulator_disable(config->veng_reg);
		if (ret) {
			LOG_ERR("Failed to disable VENG regulator");
			return ret;
		}
		k_msleep(10);
	}

	if (config->vpos_reg) {
		ret = regulator_disable(config->vpos_reg);
		if (ret) {
			LOG_ERR("Failed to disable VPOS regulator");
			return ret;
		}
		k_msleep(10);
	}

	if (config->vdd_reg) {
		ret = regulator_disable(config->vdd_reg);
		if (ret) {
			LOG_ERR("Failed to disable VDD regulator");
			return ret;
		}
		k_msleep(10);
	}

	return ret;
}
static int vga020_pm_action(const struct device *dev, enum pm_device_action action)
{
	int ret = 0;
	struct vga020_data *data = dev->data;

	pm_device_state_lock(dev);
	switch (action) {
	case PM_DEVICE_ACTION_SUSPEND:
		if (data->is_initialized) {
			ret = vga020_power_off(dev);
			if (ret == 0) {
				data->is_initialized = false; /* Will re-init on resume */
			}
		}
		break;
	case PM_DEVICE_ACTION_RESUME:
		if (!data->is_initialized) {
			ret = vga020_hw_init(dev);
			if (ret != 0) {
				LOG_ERR("Hardware init failed on resume: %d", ret);
			}
		}
		break;
	default:
		break;
	}

	pm_device_state_unlock(dev);
	return ret;
}
#endif

/* Custom API for panning control */
int vga020_set_pan(const struct device *dev, int8_t x_offset, int8_t y_offset)
{
	const struct vga020_config *config = dev->config;
	struct vga020_data *data = dev->data;
	int ret = 0;

	if (!data->is_initialized) {
		LOG_ERR("Display not initialized");
		return -ENODEV;
	}

	/* Clamp offsets to [-50, 50] range */
	if (x_offset < -50) {
		x_offset = -50;
	} else if (x_offset > 50) {
		x_offset = 50;
	}

	if (y_offset < -50) {
		y_offset = -50;
	} else if (y_offset > 50) {
		y_offset = 50;
	}

	/* Calculate centering offsets with pan adjustment
	 * x_offset < 0: shift left (decrease left border, increase right border)
	 * x_offset > 0: shift right (increase left border, decrease right border)
	 * y_offset < 0: shift up (increase top border, decrease bottom border)
	 * y_offset > 0: shift down (decrease top border, increase bottom border)
	 */
	int16_t base_top = (VGA020_PHY_HEIGHT - config->y_resolution) / 2;
	int16_t base_left = (VGA020_PHY_WIDTH - config->x_resolution) / 2;

	uint8_t top = base_top - y_offset;
	uint8_t bottom = VGA020_PHY_HEIGHT - config->y_resolution - top;
	uint8_t left = base_left + x_offset;
	uint8_t right = VGA020_PHY_WIDTH - config->x_resolution - left;

	/* Write new offsets to registers */
	ret = vga020_write_reg(dev, VGA020_REG_TOP, top);
	if (ret) {
		LOG_ERR("Failed to write TOP register: %d", ret);
		return ret;
	}

	ret = vga020_write_reg(dev, VGA020_REG_BOTTOM, bottom);
	if (ret) {
		LOG_ERR("Failed to write BOTTOM register: %d", ret);
		return ret;
	}

	ret = vga020_write_reg(dev, VGA020_REG_LEFT, left);
	if (ret) {
		LOG_ERR("Failed to write LEFT register: %d", ret);
		return ret;
	}

	ret = vga020_write_reg(dev, VGA020_REG_RIGHT, right);
	if (ret) {
		LOG_ERR("Failed to write RIGHT register: %d", ret);
		return ret;
	}

	LOG_DBG("Pan set: x=%d, y=%d (top=%d, bottom=%d, left=%d, right=%d)",
		x_offset, y_offset, top, bottom, left, right);

	return 0;
}

/* Device driver API*/
static const struct display_driver_api vga020_api = {
	.blanking_on = vga020_display_blanking_on,
	.blanking_off = vga020_display_blanking_off,
	.write = vga020_write,
	.read = vga020_read,
	.get_framebuffer = vga020_get_framebuffer,
	.set_brightness = vga020_set_brightness,
	.set_contrast = vga020_set_contrast,
	.get_capabilities = vga020_get_capabilities,
	.set_pixel_format = vga020_set_pixel_format,
	.set_orientation = vga020_set_orientation,
};

#define VGA020_DEFINE(inst)                                                                        \
	static struct vga020_data vga020_data_##inst;                                              \
	static const struct vga020_config vga020_config_##inst = {                                 \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.mipi_dev = DEVICE_DT_GET_OR_NULL(DT_INST_PHANDLE(inst, mipi)),                    \
		.reset_gpio = GPIO_DT_SPEC_INST_GET(inst, reset_gpios),                            \
		.vdd_reg = DEVICE_DT_GET_OR_NULL(DT_INST_PHANDLE(inst, vdd_supply)),               \
		.vpos_reg = DEVICE_DT_GET_OR_NULL(DT_INST_PHANDLE(inst, vpos_supply)),             \
		.veng_reg = DEVICE_DT_GET_OR_NULL(DT_INST_PHANDLE(inst, veng_supply)),             \
		.x_resolution = DT_INST_PROP(inst, width),                                         \
		.y_resolution = DT_INST_PROP(inst, height),                                        \
		.lazy_init = DT_INST_PROP(inst, lazy_init),                                        \
	};                                                                                         \
	static struct vga020_data vga020_data_##inst = {                                           \
		.is_initialized = false,                                                           \
		.attached = false,                                                                 \
		.brightness = 50,                                                                  \
		.pixel_format = PIXEL_FORMAT_RGB_888,                                              \
		.bytes_per_pixel = 3,                                                              \
		.orientation = DISPLAY_ORIENTATION_NORMAL,                                         \
	};                                                                                         \
	PM_DEVICE_DT_INST_DEFINE(inst, vga020_pm_action);                                          \
	DEVICE_DT_INST_DEFINE(inst, &vga020_init, PM_DEVICE_DT_INST_GET(inst),                     \
			      &vga020_data_##inst, &vga020_config_##inst, POST_KERNEL,             \
			      CONFIG_APPLICATION_INIT_PRIORITY, &vga020_api);

DT_INST_FOREACH_STATUS_OKAY(VGA020_DEFINE)