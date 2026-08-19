/*
 * Copyright (c) 2025 QST Corporation Limited
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qst_qmc6308

#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/policy.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(QMC6308, CONFIG_SENSOR_LOG_LEVEL);

/* Register addresses */
#define QMC6308_REG_CHIPID       0x00
#define QMC6308_REG_DATA_OUT_X_L 0x01
#define QMC6308_REG_DATA_OUT_X_H 0x02
#define QMC6308_REG_DATA_OUT_Y_L 0x03
#define QMC6308_REG_DATA_OUT_Y_H 0x04
#define QMC6308_REG_DATA_OUT_Z_L 0x05
#define QMC6308_REG_DATA_OUT_Z_H 0x06
#define QMC6308_REG_STATUS       0x09
#define QMC6308_REG_CONF_0       0x0A
#define QMC6308_REG_CONF_1       0x0B
#define QMC6308_REG_CTRL         0x0D
#define QMC6308_REG_SIGN         0x29

/* Register Parameter */
#define QMC6308_CHIP_ID               0x80
#define QMC6308_DATA_READY_TIMEOUT_MS 100

#define QMC6308_STATUS_DRDY_MASK    BIT(0)
#define QMC6308_STATUS_OVFL_MASK    BIT(1)
#define QMC6308_STATUS_NVM_RDT_MASK BIT(4)
#define QMC6308_STATUS_NVM_LD_MASK  BIT(5)

#define QMC6308_CONF_0_MODE_MASK 0x03
#define QMC6308_CONF_0_ODR_MASK  0x0C
#define QMC6308_CONF_0_OSR1_MASK 0x30
#define QMC6308_CONF_0_OSR2_MASK 0xC0

#define QMC6308_CONF_1_SET_MODE_MASK   0x03
#define QMC6308_CONF_1_RNG_MASK        0x0C
#define QMC6308_CONF_1_SELFTEST_MASK   0x30
#define QMC6308_CONF_1_SOFT_RESET_MASK 0xC0

#define QMC6308_CTRL_SR_MASK BIT(6)

/* Supported ranges */
enum qmc6308_range {
	QMC6308_RANGE_30GUASS = 0,
	QMC6308_RANGE_12GUASS = 1,
	QMC6308_RANGE_8GUASS = 2,
	QMC6308_RANGE_2GUASS = 3,
};

/* Supported ODR */
enum qmc6308_odr {
	QMC6308_ODR_10HZ = 0,
	QMC6308_ODR_50HZ = 1,
	QMC6308_ODR_100HZ = 2,
	QMC6308_ODR_200HZ = 3,
	QMC6308_ODR_1500HZ = 4,
};

/* Supported modes */
enum qmc6308_mode {
	QMC6308_MODE_SUSPEND = 0,
	QMC6308_MODE_NORMAL = 1,
	QMC6308_MODE_SINGLE = 2,
	QMC6308_MODE_CONTINUOUS = 3,
};

/* Supoort over sample*/
enum qmc6308_ovs {
	QMC6308_OVS_8 = 0,
	QMC6308_OVS_4 = 1,
	QMC6308_OVS_2 = 2,
	QMC6308_OVS_1 = 3,
};

/* Supported down sample */
enum qmc6308_ds {
	QMC6308_DS_1 = 0,
	QMC6308_DS_2 = 1,
	QMC6308_DS_4 = 2,
	QMC6308_DS_8 = 3,
};

struct qmc6308_config {
	struct i2c_dt_spec i2c;
	const struct device *vdd_reg;
	uint8_t range;
	uint8_t odr;
	uint8_t mode;
	uint8_t ovs;
	uint8_t ds;
	bool lazy_init;
};

struct qmc6308_data {
	int16_t magn_x;
	int16_t magn_y;
	int16_t magn_z;
	uint8_t mode;
	uint8_t range;
	uint8_t odr;
	uint8_t ovs;
	uint8_t ds;
	bool is_initialized;
};

static int qmc6308_hw_init(const struct device *dev);

static int qmc6308_read_reg(const struct device *dev, uint8_t reg, uint8_t *val)
{
	const struct qmc6308_config *cfg = dev->config;

	return i2c_reg_read_byte_dt(&cfg->i2c, reg, val);
}

static int qmc6308_write_reg(const struct device *dev, uint8_t reg, uint8_t val)
{
	const struct qmc6308_config *cfg = dev->config;

	return i2c_reg_write_byte_dt(&cfg->i2c, reg, val);
}

static int qmc6308_set_mode(const struct device *dev, enum qmc6308_mode mode)
{
	struct qmc6308_data *data = dev->data;
	uint8_t reg_val;
	int ret;
	ret = qmc6308_read_reg(dev, QMC6308_REG_CONF_0, &reg_val);
	if (ret < 0) {
		return ret;
	}
	reg_val &= ~QMC6308_CONF_0_MODE_MASK;
	reg_val |= FIELD_PREP(QMC6308_CONF_0_MODE_MASK, mode);
	ret = qmc6308_write_reg(dev, QMC6308_REG_CONF_0, reg_val);
	if (ret < 0) {
		return ret;
	}
	data->mode = mode;
	return ret;
}

static int qmc6308_set_range(const struct device *dev, enum qmc6308_range range)
{
	struct qmc6308_data *data = dev->data;
	uint8_t reg_val;
	int ret;

	ret = qmc6308_read_reg(dev, QMC6308_REG_CONF_1, &reg_val);
	if (ret < 0) {
		return ret;
	}

	reg_val &= ~QMC6308_CONF_1_RNG_MASK;
	reg_val |= FIELD_PREP(QMC6308_CONF_1_RNG_MASK, range);

	ret = qmc6308_write_reg(dev, QMC6308_REG_CONF_1, reg_val);
	if (ret < 0) {
		return ret;
	}

	data->range = range;

	return ret;
}

static int qmc6308_set_odr(const struct device *dev, enum qmc6308_odr odr)
{
	struct qmc6308_data *data = dev->data;
	uint8_t reg_val;
	int ret;

	if (odr == QMC6308_ODR_1500HZ) {
		LOG_DBG("Only continuous mode is supported for 1500Hz ODR, switching to continuous "
			"mode");
		ret = qmc6308_set_mode(dev, QMC6308_MODE_CONTINUOUS);
		if (ret < 0) {
			return ret;
		}
		data->odr = QMC6308_ODR_1500HZ;
	}

	ret = qmc6308_read_reg(dev, QMC6308_REG_CONF_0, &reg_val);
	if (ret < 0) {
		return ret;
	}

	reg_val &= ~QMC6308_CONF_0_ODR_MASK;
	reg_val |= FIELD_PREP(QMC6308_CONF_0_ODR_MASK, odr);

	ret = qmc6308_write_reg(dev, QMC6308_REG_CONF_0, reg_val);
	if (ret < 0) {
		return ret;
	}

	data->odr = odr;

	return ret;
}

static int qmc6308_set_ovs(const struct device *dev, enum qmc6308_ovs ovs)
{
	struct qmc6308_data *data = dev->data;
	uint8_t reg_val;
	int ret;
	ret = qmc6308_read_reg(dev, QMC6308_REG_CONF_0, &reg_val);
	if (ret < 0) {
		return ret;
	}
	reg_val &= ~QMC6308_CONF_0_OSR1_MASK;
	reg_val |= FIELD_PREP(QMC6308_CONF_0_OSR1_MASK, ovs);
	ret = qmc6308_write_reg(dev, QMC6308_REG_CONF_0, reg_val);
	if (ret < 0) {
		return ret;
	}
	data->ovs = ovs;
	return ret;
}

static int qmc6308_set_ds(const struct device *dev, enum qmc6308_ds ds)
{
	struct qmc6308_data *data = dev->data;
	uint8_t reg_val;
	int ret;
	ret = qmc6308_read_reg(dev, QMC6308_REG_CONF_0, &reg_val);
	if (ret < 0) {
		return ret;
	}
	reg_val &= ~QMC6308_CONF_0_OSR2_MASK;
	reg_val |= FIELD_PREP(QMC6308_CONF_0_OSR2_MASK, ds);
	ret = qmc6308_write_reg(dev, QMC6308_REG_CONF_0, reg_val);
	if (ret < 0) {
		return ret;
	}
	data->ds = ds;
	return ret;
}

static int qmc6308_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	struct qmc6308_data *data = dev->data;
	const struct qmc6308_config *cfg = dev->config;
	uint8_t buf[6];
	uint32_t start_time, current_time, elapsed;
	int ret;

	if (chan != SENSOR_CHAN_ALL && chan != SENSOR_CHAN_MAGN_XYZ) {
		return -ENOTSUP;
	}

	// if mode is single, trigger single conversion
	if (cfg->mode == QMC6308_MODE_SINGLE) {
		ret = qmc6308_set_mode(dev, QMC6308_MODE_SINGLE);
		if (ret < 0) {
			LOG_ERR("Failed to set mode to single");
			return ret;
		}
	}

	start_time = k_uptime_get();
	do {
		// check status register
		ret = qmc6308_read_reg(dev, QMC6308_REG_STATUS, &buf[0]);
		if (ret < 0) {
			LOG_ERR("Failed to read status register %d", ret);
			return ret;
		}

		if (buf[0] & QMC6308_STATUS_DRDY_MASK) {
			break; // data ready
		}

		// timeout check
		current_time = k_uptime_get();
		elapsed = current_time - start_time;

		if (elapsed > QMC6308_DATA_READY_TIMEOUT_MS) {
			LOG_ERR("Timeout waiting for data ready (%u ms)", elapsed);
			return -ETIMEDOUT;
		}
		k_usleep(100);
	} while (true);

	ret = i2c_burst_read_dt(&cfg->i2c, QMC6308_REG_DATA_OUT_X_L, buf, sizeof(buf));
	if (ret < 0) {
		LOG_ERR("Failed to read acceleration data");
		return ret;
	}

	data->magn_x = (buf[1] << 8) | buf[0];
	data->magn_y = (buf[3] << 8) | buf[2];
	data->magn_z = (buf[5] << 8) | buf[4];

	return 0;
}

static int qmc6308_channel_get(const struct device *dev, enum sensor_channel chan,
			       struct sensor_value *val)
{
	struct qmc6308_data *data = dev->data;
	const struct qmc6308_config *cfg = dev->config;
	double lsb_to_m = 1.0f;

	/* Determine scale factor based on range */
	switch (cfg->range) {
	case QMC6308_RANGE_30GUASS:
		lsb_to_m = 1000.0f;
		break;
	case QMC6308_RANGE_12GUASS:
		lsb_to_m = 2500.0f;
		break;
	case QMC6308_RANGE_8GUASS:
		lsb_to_m = 3750.0f;
		break;
	case QMC6308_RANGE_2GUASS:
		lsb_to_m = 15000.0f;
		break;
	}

	switch (chan) {
	case SENSOR_CHAN_MAGN_X:
		sensor_value_from_double(val, data->magn_x / lsb_to_m);
		break;
	case SENSOR_CHAN_MAGN_Y:
		sensor_value_from_double(val, data->magn_y / lsb_to_m);
		break;
	case SENSOR_CHAN_MAGN_Z:
		sensor_value_from_double(val, data->magn_z / lsb_to_m);
		break;
	case SENSOR_CHAN_MAGN_XYZ:
		sensor_value_from_double(val, data->magn_x / lsb_to_m);
		sensor_value_from_double(val + 1, data->magn_y / lsb_to_m);
		sensor_value_from_double(val + 2, data->magn_z / lsb_to_m);
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}

static inline enum qmc6308_odr qmc6308_find_odr(const struct sensor_value *val)
{
	enum qmc6308_odr odr;
	if (val->val1 < 50) {
		odr = QMC6308_ODR_10HZ;
	} else if (val->val1 < 100) {
		odr = QMC6308_ODR_50HZ;
	} else if (val->val1 < 200) {
		odr = QMC6308_ODR_100HZ;
	} else if (val->val1 < 1500) {
		odr = QMC6308_ODR_200HZ;
	} else {
		odr = QMC6308_ODR_1500HZ;
	}
	return odr;
}
static inline enum qmc6308_range qmc6308_find_range(const struct sensor_value *val)
{
	enum qmc6308_range range;
	if (val->val1 <= 2) {
		range = QMC6308_RANGE_2GUASS;
	} else if (val->val1 <= 8) {
		range = QMC6308_RANGE_8GUASS;
	} else if (val->val1 <= 12) {
		range = QMC6308_RANGE_12GUASS;
	} else {
		range = QMC6308_RANGE_30GUASS;
	}
	return range;
}

static int qmc6308_attr_set(const struct device *dev, enum sensor_channel chan,
			    enum sensor_attribute attr, const struct sensor_value *val)
{
	const struct qmc6308_config *cfg = dev->config;
	enum qmc6308_odr odr = cfg->odr;
	enum qmc6308_range range = cfg->range;
	int ret;

	if (chan != SENSOR_CHAN_MAGN_XYZ && chan != SENSOR_CHAN_ALL) {
		return -ENOTSUP;
	}

	switch (attr) {
	case SENSOR_ATTR_SAMPLING_FREQUENCY:
		odr = qmc6308_find_odr(val);
		ret = qmc6308_set_odr(dev, odr);
		if (ret < 0) {
			LOG_ERR("Failed to set ODR");
			return ret;
		}
		break;
	case SENSOR_ATTR_FULL_SCALE:
		range = qmc6308_find_range(val);
		ret = qmc6308_set_range(dev, range);
		if (ret < 0) {
			LOG_ERR("Failed to set range");
			return ret;
		}
	default:
		return -ENOTSUP;
	}
	return 0;
}

static int qmc6308_hw_init(const struct device *dev)
{
	const struct qmc6308_config *cfg = dev->config;
	struct qmc6308_data *data = dev->data;
	uint8_t chip_id;
	uint8_t reg_val;
	int ret;

	if (data->is_initialized) {
		return 0; /* Already initialized */
	}

	// Enable power if not already enabled (for lazy init mode)
	if (cfg->vdd_reg) {
		ret = regulator_enable(cfg->vdd_reg);
		if (ret < 0) {
			LOG_ERR("Failed to enable regulator");
			return ret;
		}
	}

	// Wait for power to stabilize - reduced from 5ms to 2ms for faster boot
	k_sleep(K_MSEC(2));

	/* Verify chip ID */
	ret = qmc6308_read_reg(dev, QMC6308_REG_CHIPID, &chip_id);
	if (ret < 0) {
		LOG_ERR("Failed to read chip ID");
		return ret;
	}

	if (chip_id != QMC6308_CHIP_ID) { // Update with correct expected value
		LOG_ERR("Invalid chip ID 0x%02x", chip_id);
		return -EINVAL;
	}

	/* Soft reset */
	ret = qmc6308_write_reg(dev, QMC6308_REG_CONF_1, 0x80);
	if (ret < 0) {
		LOG_ERR("Soft reset failed");
		return ret;
	}

	k_sleep(K_MSEC(5));

	/* Check if soft reset was successful */
	ret = qmc6308_read_reg(dev, QMC6308_REG_CONF_1, &reg_val);
	if (ret < 0) {
		LOG_ERR("Failed to read soft reset status");
		return ret;
	}
	if (reg_val != 0x80) {
		LOG_ERR("Soft reset failed");
		return -EIO;
	}

	// clear status register
	ret = qmc6308_write_reg(dev, QMC6308_REG_CONF_1, 0x00);
	if (ret < 0) {
		LOG_ERR("Failed to clear status register");
		return ret;
	}

	/* Configure range */
	ret = qmc6308_set_range(dev, cfg->range);
	if (ret < 0) {
		LOG_ERR("Failed to set range");
		return ret;
	}

	/* Configure ODR */
	ret = qmc6308_set_odr(dev, cfg->odr);
	if (ret < 0) {
		LOG_ERR("Failed to set ODR");
		return ret;
	}

	/* Configure over sample */
	ret = qmc6308_set_ovs(dev, cfg->ovs);
	if (ret < 0) {
		LOG_ERR("Failed to set over sample");
		return ret;
	}

	/* Configure down sample */
	ret = qmc6308_set_ds(dev, cfg->ds);
	if (ret < 0) {
		LOG_ERR("Failed to set down sample");
		return ret;
	}

	/* Configure mode */
	ret = qmc6308_set_mode(dev, cfg->mode);
	if (ret < 0) {
		LOG_ERR("Failed to set mode");
		return ret;
	}

	data->is_initialized = true;

	return 0;
}

static int qmc6308_init(const struct device *dev)
{
	const struct qmc6308_config *cfg = dev->config;
	struct qmc6308_data *data = dev->data;

	if (!i2c_is_ready_dt(&cfg->i2c)) {
		LOG_ERR("I2C device not ready");
		return -ENODEV;
	}

	if (cfg->vdd_reg && !device_is_ready(cfg->vdd_reg)) {
		LOG_ERR("Regulator not ready");
		return -ENODEV;
	}

	/* Always mark device as suspended - hardware will be initialized on resume */
	data->is_initialized = false;
	
#ifdef CONFIG_PM_DEVICE
	pm_device_init_suspended(dev);
#endif
	
	if (cfg->lazy_init) {
		LOG_INF("QMC6308 init done (lazy-init mode: hardware init deferred until resume)");
	} else {
		LOG_INF("QMC6308 init done (normal mode: hardware init deferred until resume)");
	}

	return 0;
}

#ifdef CONFIG_PM_DEVICE

static int qmc6308_power_off(const struct device *dev)
{
	const struct qmc6308_config *cfg = dev->config;
	int ret = 0;
	if (cfg->vdd_reg) {
		ret = regulator_disable(cfg->vdd_reg);
	}
	return ret;
}
static int qmc6308_pm_action(const struct device *dev, enum pm_device_action action)
{

	int ret = 0;
	struct qmc6308_data *data = dev->data;
	
	pm_device_state_lock(dev);
	switch (action) {
	case PM_DEVICE_ACTION_SUSPEND:
		if (data->is_initialized) {
			ret = qmc6308_set_mode(dev, QMC6308_MODE_SUSPEND);
			if (ret == 0) {
				data->is_initialized = false; /* Will re-init on resume */
			}
		}
		break;
	case PM_DEVICE_ACTION_RESUME:
		if (!data->is_initialized) {
			ret = qmc6308_hw_init(dev);
			if (ret != 0) {
				LOG_ERR("Hardware init failed on resume: %d", ret);
				pm_device_state_unlock(dev);
				return ret;
			}
		}
		break;
	case PM_DEVICE_ACTION_TURN_OFF:
		ret = qmc6308_power_off(dev);
		break;
	case PM_DEVICE_ACTION_TURN_ON:
		ret = qmc6308_hw_init(dev);
		break;
	default:
		ret = -ENOTSUP;
		break;
	}
	pm_device_state_unlock(dev);
	return ret;
}
#endif

static const struct sensor_driver_api qmc6308_api = {
	.attr_set = qmc6308_attr_set,
	.sample_fetch = qmc6308_sample_fetch,
	.channel_get = qmc6308_channel_get,
};

#define QMC6308_DEFINE(inst)                                                                       \
	static struct qmc6308_data qmc6308_data_##inst;                                            \
	static const struct qmc6308_config qmc6308_config_##inst = {                               \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.vdd_reg = DEVICE_DT_GET_OR_NULL(DT_INST_PHANDLE(inst, vin_supply)),               \
		.range = DT_INST_ENUM_IDX(inst, range),                                            \
		.odr = DT_INST_ENUM_IDX(inst, odr),                                                \
		.mode = DT_INST_ENUM_IDX(inst, mode),                                              \
		.ovs = DT_INST_ENUM_IDX(inst, ovs),                                                \
		.ds = DT_INST_ENUM_IDX(inst, ds),                                                  \
		.lazy_init = DT_INST_PROP_OR(inst, lazy_init, false),                              \
	};                                                                                         \
	PM_DEVICE_DT_INST_DEFINE(inst, qmc6308_pm_action);                                         \
	DEVICE_DT_INST_DEFINE(inst, &qmc6308_init, PM_DEVICE_DT_INST_GET(inst),                    \
			      &qmc6308_data_##inst, &qmc6308_config_##inst, POST_KERNEL,           \
			      CONFIG_SENSOR_INIT_PRIORITY, &qmc6308_api);

DT_INST_FOREACH_STATUS_OKAY(QMC6308_DEFINE)