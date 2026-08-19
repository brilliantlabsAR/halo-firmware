/* Copyright (c) 2025 PixArt Imaging Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT pixart_pag7982
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/policy.h>
#include <zephyr/drivers/video.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/regulator.h>

#define LOG_LEVEL CONFIG_LOG_DEFAULT_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(pag7982, CONFIG_VIDEO_LOG_LEVEL);

#include "pag7982.h"

struct pag7982_reg {
	uint8_t addr;
	uint8_t value;
};

/* 24M 640 * 480 */
static const struct pag7982_reg default_regs[] = {
	{0xEF, 0xA5}, {0xEF, 0x04}, {0x0F, 0x10}, {0xEF, 0x00}, {0xEE, 0x06}, {0x45, 0x44},
	{0x0C, 0xC0}, {0x11, 0xC0}, {0x42, 0xC0}, {0x43, 0xC0}, {0x44, 0xC0}, {0x4C, 0x80},
	{0x4D, 0x1A}, {0x4E, 0x06}, {0x4F, 0x00}, {0xAF, 0x00}, {0x09, 0x00}, {0x16, 0x0C},
	{0x18, 0x00}, {0x19, 0x80}, {0x2D, 0x43}, {0x38, 0x08}, {0x3A, 0x57}, {0x3B, 0x6B},
	{0x66, 0x01}, {0x67, 0x02}, {0xE6, 0x10}, {0xEF, 0x01}, {0x0C, 0x02}, {0x19, 0x81},
	{0x1A, 0x06}, {0x1B, 0x81}, {0x1C, 0x06}, {0x30, 0x08}, {0x31, 0x02}, {0x32, 0x44},
	{0x35, 0x0A}, {0x3B, 0x30}, {0x3F, 0x90}, {0x40, 0x00}, {0x46, 0x06}, {0x63, 0x05},
	{0x64, 0x05}, {0x65, 0x06}, {0x66, 0x06}, {0x69, 0xD0}, {0x6B, 0xD0}, {0x76, 0x07},
	{0x77, 0x08}, {0x78, 0x04}, {0x79, 0x05}, {0x7A, 0x43}, {0x7B, 0x43}, {0x85, 0x2D},
	{0x87, 0x20}, {0x89, 0x2D}, {0x8B, 0x20}, {0xD9, 0x18}, {0xDB, 0x18}, {0xDD, 0x40},
	{0xE2, 0x01}, {0xE4, 0x22}, {0xEF, 0x02}, {0x92, 0x11}, {0x93, 0x01}, {0xBE, 0x72},
	{0xC0, 0x72}, {0xA5, 0x90}, {0xA6, 0x00}, {0xD0, 0x3E}, {0x11, 0x03}, {0x02, 0xBF},
	{0x03, 0x00}, {0x04, 0x08}, {0x5B, 0x01}, {0x31, 0xBF}, {0x32, 0x00}, {0xDF, 0x0B},
	{0xDD, 0x00}, {0xDE, 0x00}, {0xE8, 0x01}, {0xE9, 0x07}, {0xE7, 0x03}, {0xDF, 0x8B},
	{0xEA, 0x0A}, {0xEB, 0x04}, {0xEC, 0x00}, {0xED, 0x04}, {0x21, 0x80}, {0x22, 0x02},
	{0x23, 0xE0}, {0x24, 0x01}, {0x25, 0x08}, {0x26, 0x00}, {0x27, 0x16}, {0x28, 0x00},
	{0x58, 0xFE}, {0x59, 0x01}, {0xEF, 0x04}, {0x3A, 0xA0}, {0x3B, 0x78}, {0x6E, 0xD2},
	{0x70, 0x80}, {0x72, 0x04}, {0x73, 0x01}, {0x30, 0x31}, {0x40, 0x1D}, {0x41, 0x00},
	{0x42, 0xC0}, {0x43, 0x00}, {0x44, 0xC8}, {0x45, 0x00}, {0x46, 0x00}, {0x47, 0x00},
	{0x48, 0x20}, {0x49, 0x11}, {0x4A, 0x06}, {0x4B, 0x00}, {0x50, 0x22}, {0xEF, 0x00},
	{0x09, 0x22}, {0xEF, 0x01}, {0xC2, 0xF7}, {0xC3, 0x01}, {0xCB, 0xF3}, {0xCC, 0x01},
	{0xCE, 0x0C}, {0xEF, 0x02}, {0x27, 0x14}, {0xD9, 0x02}, {0xEF, 0x00}, {0xEB, 0x80},
	{0xEF, 0x00}, {0x30, 0x00}, {0x1D, 0x80}, {0x64, 0x02}, {0xEF, 0x01}, {0x69, 0x68},
	{0x6B, 0x68}, {0x7A, 0x22}, {0x7B, 0x22}, {0x84, 0xB6}, {0x88, 0xB6}, {0x32, 0x44},
	{0x30, 0x08}, {0x31, 0x02}, {0x35, 0x0A}, {0x3B, 0x30}, {0x0C, 0x02}, {0x3F, 0x90},
	{0x40, 0x00}, {0x46, 0x06}, {0xD9, 0x18}, {0xDB, 0x18}, {0xDD, 0x40}, {0xEF, 0x02},
	{0xA5, 0x90}, {0xA6, 0x00},
};

// /* 48M 640x480 */
// static const struct pag7982_reg default_regs[] = {
// 	{0xEF, 0x00}, {0x45, 0x44}, {0x0c, 0xC0}, {0x11, 0xC0}, {0x42, 0xC0}, {0x43, 0xC0},
// 	{0x44, 0xC0}, {0xEF, 0x00}, {0x26, 0x08}, {0x2B, 0x00}, {0x36, 0x82}, {0x3C, 0x24},
// 	{0x36, 0x81}, {0x65, 0x12}, {0xEF, 0x00}, {0x4C, 0x00}, {0x4D, 0x35}, {0x4E, 0x0C},
// 	{0x4F, 0x00}, {0xAF, 0x01}, {0xEF, 0x04}, {0x30, 0x31}, {0x40, 0x1D}, {0x41, 0x00},
// 	{0x42, 0xD0}, {0x43, 0x01}, {0x44, 0xC8}, {0x45, 0x00}, {0x46, 0x00}, {0x47, 0x00},
// 	{0x48, 0xE0}, {0x49, 0x03}, {0x4A, 0x03}, {0x4B, 0x00}, {0xEF, 0x02}, {0x11, 0x03},
// 	{0xEF, 0x02}, {0x02, 0xBF}, {0x03, 0x00}, {0x04, 0x08}, {0x5B, 0x01}, {0x31, 0xBF},
// 	{0x32, 0x00}, {0xEF, 0x02}, {0xDF, 0x0B}, {0xDD, 0x00}, {0xDE, 0x00}, {0xE8, 0x02},
// 	{0xE9, 0x06}, {0xE7, 0x03}, {0xDB, 0x7F}, {0xDC, 0x00}, {0xEF, 0x04}, {0x50, 0x22},
// 	{0xEF, 0x00}, {0x09, 0x00}, {0x18, 0x06}, {0x19, 0x80}, {0x2D, 0x43}, {0x38, 0x08},
// 	{0x3A, 0x57}, {0x3B, 0x6B}, {0x66, 0x01}, {0x67, 0x02}, {0xEF, 0x01}, {0x19, 0x81},
// 	{0x1A, 0x06}, {0x1B, 0x81}, {0x1C, 0x06}, {0x65, 0x06}, {0x66, 0x06}, {0x69, 0xD0},
// 	{0x6B, 0xD0}, {0x76, 0x07}, {0x77, 0x08}, {0x78, 0x04}, {0x79, 0x05}, {0x7A, 0x43},
// 	{0x7B, 0x43}, {0x85, 0x2D}, {0x87, 0x20}, {0x89, 0x2D}, {0x8B, 0x20}, {0xE2, 0x01},
// 	{0xEF, 0x02}, {0x92, 0x11}, {0x93, 0x01}, {0xBE, 0x72}, {0xC0, 0x72}, {0xD0, 0x3E},
// 	{0xEF, 0x02}, {0x21, 0x80}, {0x22, 0x02}, {0x23, 0xE0}, {0x24, 0x01}, {0x25, 0x08},
// 	{0x26, 0x00}, {0x27, 0x16}, {0x28, 0x00}, {0x58, 0xFE}, {0x59, 0x01}, {0xEF, 0x04},
// 	{0x3A, 0xA0}, {0x3B, 0x78}, {0xEF, 0x00}, {0xEB, 0x80}, {0xEF, 0x00}, {0x30, 0x00},
// };

static int pag7982_hw_init(const struct device *dev);

struct pag7982_config {
	struct i2c_dt_spec i2c;
	struct gpio_dt_spec reset;
	struct gpio_dt_spec power;
	const struct device *vdd_reg;
	uint32_t pixclk;
	uint32_t frame_rate;
	uint8_t drop; // drop frame
	bool lazy_init;
};

struct pag7982_data {
	struct video_format fmt;
	bool is_initialized;
};

#define PAG7982_VIDEO_FORMAT_CAP(width, height, format, w_step, h_step)                            \
	{.pixelformat = (format),                                                                  \
	 .width_min = (width),                                                                     \
	 .width_max = (width),                                                                     \
	 .height_min = (height),                                                                   \
	 .height_max = (height),                                                                   \
	 .width_step = 0,                                                                          \
	 .height_step = 0}

static const struct video_format_cap fmts[] = {
	// PAG7982_VIDEO_FORMAT_CAP(160, 120, VIDEO_PIX_FMT_BGGR8, 4, 0),  /* QQVGA */
	// PAG7982_VIDEO_FORMAT_CAP(160, 120, VIDEO_PIX_FMT_BGGR8, 0, 4),  /* QQVGA */
	// PAG7982_VIDEO_FORMAT_CAP(320, 240, VIDEO_PIX_FMT_BGGR8, 2, 0), /* QVGA  */
	// PAG7982_VIDEO_FORMAT_CAP(320, 240, VIDEO_PIX_FMT_BGGR8, 0, 2), /* QVGA  */
	PAG7982_VIDEO_FORMAT_CAP(640, 480, VIDEO_PIX_FMT_BGGR8, 0, 0), /* VGA   */
	{0}};

static int pag7982_write_reg(const struct i2c_dt_spec *spec, uint8_t reg_addr, uint8_t value)
{
	uint8_t tries = 3;
	/**
	 * It rarely happens that the camera does not respond with ACK signal.
	 * In that case it usually responds on 2nd try but there is a 3rd one
	 * just to be sure that the connection error is not caused by driver
	 * itself.
	 */
	while (tries--) {
		if (!i2c_reg_write_byte_dt(spec, reg_addr, value)) {
			return 0;
		}
		/* If writing failed wait 5ms before next attempt */
		k_msleep(5);
	}
	LOG_ERR("failed to write 0x%x to 0x%x", value, reg_addr);

	return -1;
}

static int pag7982_read_reg(const struct i2c_dt_spec *spec, uint8_t reg_addr)
{
	uint8_t tries = 3;
	uint8_t value;

	/**
	 * It rarely happens that the camera does not respond with ACK signal.
	 * In that case it usually responds on 2nd try but there is a 3rd one
	 * just to be sure that the connection error is not caused by driver
	 * itself.
	 */
	while (tries--) {
		if (!i2c_reg_read_byte_dt(spec, reg_addr, &value)) {
			return value;
		}
		/* If reading failed wait 5ms before next attempt */
		k_msleep(10);
	}
	LOG_ERR("failed to read 0x%x register", reg_addr);

	return -1;
}

static int pag7982_write_all(const struct device *dev, const struct pag7982_reg *regs,
			     uint16_t reg_num)
{
	uint16_t i = 0;
	const struct pag7982_config *config = dev->config;

	for (i = 0; i < reg_num; i++) {
		int err;

		err = pag7982_write_reg(&config->i2c, regs[i].addr, regs[i].value);
		if (err) {
			return err;
		}
	}

	return 0;
}

int pag7982_check_connection(const struct device *dev)
{
	int ret = 0;
	const struct pag7982_config *config = dev->config;

	uint8_t part_id_l, part_id_h;

	ret |= pag7982_write_reg(&config->i2c, BANK_SEL, 0x00);

	/* Read part ID */
	part_id_l = pag7982_read_reg(&config->i2c, PART_ID_L);
	part_id_h = pag7982_read_reg(&config->i2c, PART_ID_H);

	if (PAG7982_ID_L != part_id_l || PAG7982_ID_H != part_id_h) {
		LOG_ERR("PAG7982 camera not found! %02X%02x", part_id_h, part_id_l);
		return -ENODEV;
	}

	LOG_DBG("PAG7982 camera found! %02X%02X", part_id_h, part_id_l);

	return ret;
}

static int pag7982_set_horizontal_mirror(const struct device *dev, int enable)
{
	int ret = 0;
	const struct pag7982_config *config = dev->config;
	uint8_t reg;

	/* Switch to SENSOR register bank */
	ret |= pag7982_write_reg(&config->i2c, BANK_SEL, 0x01);

	/* Update REG04 to enable/disable horizontal mirror */
	reg = pag7982_read_reg(&config->i2c, R_FLIP);

	if (enable) {
		reg |= 0x08;
	} else {
		reg &= ~0x08;
	}

	ret |= pag7982_write_reg(&config->i2c, R_FLIP, reg);

	ret |= pag7982_write_reg(&config->i2c, BANK_SEL, 0x00);
	ret |= pag7982_write_reg(&config->i2c, UPDATE, 0x80);

	return ret;
}

static int pag7982_set_vertical_flip(const struct device *dev, int enable)
{
	int ret = 0;
	const struct pag7982_config *config = dev->config;

	uint8_t reg;

	/* Switch to SENSOR register bank */
	ret |= pag7982_write_reg(&config->i2c, BANK_SEL, 0x01);

	/* Update REG04 to enable/disable vertical flip */
	reg = pag7982_read_reg(&config->i2c, R_FLIP);

	if (enable) {
		reg |= 0x04;
	} else {
		reg &= ~0x04;
	}

	ret |= pag7982_write_reg(&config->i2c, R_FLIP, reg);

	ret |= pag7982_write_reg(&config->i2c, BANK_SEL, 0x00);
	ret |= pag7982_write_reg(&config->i2c, UPDATE, 0x80);

	return ret;
}

static int pag7982_set_resolution(const struct device *dev, uint16_t img_width, uint16_t img_height)
{
	int ret = 0;

	uint16_t w = img_width;
	uint16_t h = img_height;

	if (w != 640 || h != 480) {
		LOG_ERR("Unsupported resolution: %dx%d", w, h);
		return -ENOTSUP;
	}

	return ret;
}
static int pag7982_set_fmt(const struct device *dev, enum video_endpoint_id ep,
			   struct video_format *fmt)
{
	struct pag7982_data *drv_data = dev->data;
	uint16_t width, height;
	int ret = 0;
	int i = 0;

	/* We only support RGB565 and JPEG pixel formats */
	if (fmt->pixelformat != VIDEO_PIX_FMT_BGGR8) {
		LOG_ERR("pag7982 camera supports only BGGR8 format");
		return -ENOTSUP;
	}

	width = fmt->width;
	height = fmt->height;

	if (!memcmp(&drv_data->fmt, fmt, sizeof(drv_data->fmt))) {
		/* nothing to do */
		return 0;
	}

	drv_data->fmt = *fmt;

	/* Check if camera is capable of handling given format */
	while (fmts[i].pixelformat) {
		if (fmts[i].width_min == width && fmts[i].height_min == height &&
		    fmts[i].pixelformat == fmt->pixelformat) {
			/* Set window size */
			ret |= pag7982_set_resolution(dev, fmt->width, fmt->height);
			return ret;
		}
		i++;
	}

	/* Camera is not capable of handling given format */
	LOG_ERR("Image format not supported\n");
	return -ENOTSUP;
}

static int pag7982_get_fmt(const struct device *dev, enum video_endpoint_id ep,
			   struct video_format *fmt)
{
	struct pag7982_data *drv_data = dev->data;

	*fmt = drv_data->fmt;

	return 0;
}

static int pag7982_hw_reset(const struct device *dev)
{
	const struct pag7982_config *config = dev->config;
	struct pag7982_data *data = dev->data;

	if (config->reset.port == NULL) {
		return -ENODEV;
	}

	gpio_pin_set_dt(&config->reset, 1U);
	k_msleep(150);
	gpio_pin_set_dt(&config->reset, 0U);
	k_msleep(200);

	data->is_initialized = false;

	return 0;
}

static int pag7982_hw_power_on(const struct device *dev)
{
	int ret = 0;
	const struct pag7982_config *config = dev->config;

	if (config->reset.port != NULL) {
		gpio_pin_set_dt(&config->reset, 1U);
	}

	// enable power
	if (config->vdd_reg) {
		ret = regulator_enable(config->vdd_reg);
		if (ret < 0) {
			LOG_ERR("Failed to enable regulator");
			return ret;
		}
	}

	if (config->power.port != NULL) {
		ret = gpio_pin_set_dt(&config->power, 1U);
		if (ret < 0) {
			LOG_ERR("Failed to enable power");
			return ret;
		}
	}
	return 0;
}

static int pag7982_hw_power_off(const struct device *dev)
{
	int ret = 0;
	const struct pag7982_config *config = dev->config;
	struct pag7982_data *data = dev->data;

	if (config->reset.port != NULL) {
		gpio_pin_set_dt(&config->reset, 1U);
	}

	if (config->vdd_reg) {
		ret = regulator_disable(config->vdd_reg);
		if (ret < 0) {
			LOG_ERR("Failed to disable regulator");
			return ret;
		}
	}

	if (config->power.port != NULL) {
		ret = gpio_pin_set_dt(&config->power, 0U);
		if (ret < 0) {
			LOG_ERR("Failed to disable power");
			return ret;
		}
	}

	data->is_initialized = false;

	return 0;
}

static int pag7982_hw_init(const struct device *dev)
{
	struct video_format fmt;
	const struct pag7982_config *config = dev->config;
	struct pag7982_data *data = dev->data;
	int ret = 0;

	if (data->is_initialized) {
		return 0; /* Already initialized */
	}

	// Enable power and reset for lazy init mode
	pag7982_hw_power_on(dev);
	pag7982_hw_reset(dev);

	ret = pag7982_check_connection(dev);

	if (ret) {
		LOG_ERR("Failed to check camera connection: %d", ret);
		pag7982_hw_power_off(dev);
		return ret;
	}

	k_msleep(5);

	pag7982_write_all(dev, default_regs, ARRAY_SIZE(default_regs));

	uint32_t frame_time = config->pixclk / (config->frame_rate * 2);
	LOG_DBG("pixclk %d frame_rate %d frame_time: %d\n", config->pixclk, config->frame_rate,
		frame_time);

	pag7982_write_reg(&config->i2c, BANK_SEL, 0x00);
	pag7982_write_reg(&config->i2c, R_FRAMETIME_0, frame_time & 0xff);
	pag7982_write_reg(&config->i2c, R_FRAMETIME_1, (frame_time >> 8) & 0xff);
	pag7982_write_reg(&config->i2c, R_FRAMETIME_2, (frame_time >> 16) & 0xff);
	pag7982_write_reg(&config->i2c, R_FRAMETIME_3, (frame_time >> 24) & 0xff);

	// test partten
	// pag7982_write_reg(&config->i2c, BANK_SEL, 0x04);
	// pag7982_write_reg(&config->i2c, 0x00, 0x01);
	// pag7982_write_reg(&config->i2c, 0x0A, 0x04);

	data->is_initialized = true;

	/* set default/init format SVGA RGB565 */
	fmt.pixelformat = VIDEO_PIX_FMT_BGGR8;
	fmt.width = 640;
	fmt.height = 480;
	fmt.pitch = 640;
	ret = pag7982_set_fmt(dev, VIDEO_EP_OUT, &fmt);
	if (ret) {
		LOG_ERR("Unable to configure default format");
		pag7982_hw_power_off(dev);
		return -EIO;
	}

	return ret;
}

static int pag7982_stream_start(const struct device *dev)
{
	int ret = 0;
	const struct pag7982_config *config = dev->config;

	// ret |= pag7982_write_reg(&config->i2c, BANK_SEL, 0x04);
	// ret |= pag7982_write_reg(&config->i2c, 0x00, 0x01);
	// ret |= pag7982_write_reg(&config->i2c, 0x0A, 0x10);

	ret |= pag7982_write_reg(&config->i2c, BANK_SEL, 0x00);
	ret |= pag7982_write_reg(&config->i2c, R_TRG_EN, 0x01);
	// ret |= pag7982_write_reg(&config->i2c, R_TRG_MODE, 0x01);
	// ret |= pag7982_write_reg(&config->i2c, R_TRG_FRAME, 0x01);
	// ret |= pag7982_write_reg(&config->i2c, 0xEA, 0x01);

	return ret;
}

static int pag7982_stream_stop(const struct device *dev)
{
	int ret = 0;

	const struct pag7982_config *config = dev->config;

	ret |= pag7982_write_reg(&config->i2c, BANK_SEL, 0x00);
	ret |= pag7982_write_reg(&config->i2c, R_TRG_EN, 0x00);

	return 0;
}

static int pag7982_get_caps(const struct device *dev, enum video_endpoint_id ep,
			    struct video_caps *caps)
{
	caps->format_caps = fmts;
	return 0;
}

static int pag7982_set_ctrl(const struct device *dev, unsigned int cid, void *value)
{
	int ret = 0;

	switch (cid) {
	case VIDEO_CID_HFLIP:
		ret |= pag7982_set_horizontal_mirror(dev, (int)value);
		break;
	case VIDEO_CID_VFLIP:
		ret |= pag7982_set_vertical_flip(dev, (int)value);
		break;
	default:
		return -ENOTSUP;
	}

	return ret;
}

static const struct video_driver_api pag7982_driver_api = {
	.set_format = pag7982_set_fmt,
	.get_format = pag7982_get_fmt,
	.get_caps = pag7982_get_caps,
	.stream_start = pag7982_stream_start,
	.stream_stop = pag7982_stream_stop,
	.set_ctrl = pag7982_set_ctrl,
};

/* Unique Instance */
static struct pag7982_data pag7982_data_0;

static int pag7982_init(const struct device *dev)
{
	const struct pag7982_config *config = dev->config;
	struct pag7982_data *data = dev->data;
	int ret = 0;

	if (!device_is_ready(config->i2c.bus)) {
		LOG_ERR("Bus device is not ready");
		return -ENODEV;
	}

	if (config->vdd_reg && !device_is_ready(config->vdd_reg)) {
		LOG_ERR("Regulator not ready");
		return -ENODEV;
	}

	if (config->power.port != NULL) {
		if (!device_is_ready(config->power.port)) {
			LOG_ERR("Power GPIO device not ready");
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(&config->power, GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			LOG_ERR("Could not configure power GPIO (%d)", ret);
		}
	}
	if (config->reset.port != NULL) {
		if (!device_is_ready(config->reset.port)) {
			LOG_ERR("Reset GPIO device not ready");
			return -ENODEV;
		}

		ret = gpio_pin_configure_dt(&config->reset, GPIO_OUTPUT_ACTIVE);
		if (ret < 0) {
			LOG_ERR("Could not configure reset GPIO (%d)", ret);
			return ret;
		}

		gpio_pin_set_dt(&config->reset, 1U);
	}
	
	/* Always mark device as suspended - hardware will be initialized on resume */
	data->is_initialized = false;

#ifdef CONFIG_PM_DEVICE
	pm_device_init_suspended(dev);
#endif

	if (config->lazy_init) {
		LOG_INF("PAG7982 init done (lazy-init mode: hardware init deferred until resume)");
	} else {
		LOG_INF("PAG7982 init done (normal mode: hardware init deferred until resume)");
	}

	return ret;
}

#ifdef CONFIG_PM_DEVICE

static int pag7982_pm_action(const struct device *dev, enum pm_device_action action)
{
	int ret = 0;
	struct pag7982_data *data = dev->data;

	pm_device_state_lock(dev);
	switch (action) {
	case PM_DEVICE_ACTION_SUSPEND:
		if (data->is_initialized) {
			ret = pag7982_hw_power_off(dev);
			if (ret == 0) {
				data->is_initialized = false; /* Will re-init on resume */
			}
		}
		break;
	case PM_DEVICE_ACTION_RESUME:
		if (!data->is_initialized) {
			ret = pag7982_hw_init(dev);
			if (ret != 0) {
				LOG_ERR("Hardware init failed on resume: %d", ret);
			}
		}
		break;
	default:
		ret = -ENOTSUP;
		break;
	}
	pm_device_state_unlock(dev);
	return ret;
}
#endif

#define PAG7982_INIT(inst)                                                                         \
	static struct pag7982_data pag7982_data_##inst;                                            \
	static const struct pag7982_config pag7982_config_##inst = {                               \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.pixclk = DT_INST_PROP(inst, pixclk),                                              \
		.frame_rate = DT_INST_PROP(inst, frame_rate),                                      \
		.reset = GPIO_DT_SPEC_INST_GET_OR(inst, reset_gpios, {0}),                         \
		.power = GPIO_DT_SPEC_INST_GET_OR(inst, power_gpios, {0}),                         \
		.vdd_reg = DEVICE_DT_GET_OR_NULL(DT_INST_PHANDLE(inst, vin_supply)),               \
		.lazy_init = DT_INST_PROP_OR(inst, lazy_init, false),                              \
	};                                                                                         \
	PM_DEVICE_DT_INST_DEFINE(inst, pag7982_pm_action);                                         \
	DEVICE_DT_INST_DEFINE(inst, pag7982_init, PM_DEVICE_DT_INST_GET(inst),                     \
			      &pag7982_data_##inst, &pag7982_config_##inst, POST_KERNEL,           \
			      CONFIG_VIDEO_INIT_PRIORITY, &pag7982_driver_api);

DT_INST_FOREACH_STATUS_OKAY(PAG7982_INIT)
