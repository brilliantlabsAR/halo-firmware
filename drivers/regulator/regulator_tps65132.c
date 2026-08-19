/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT ti_tps65132

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/sys/linear_range.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(regulator_tps65132, CONFIG_REGULATOR_LOG_LEVEL);

#define TPS65132_REG_VPOS  0x00
#define TPS65132_REG_VENG  0x01
#define TPS65132_REG_DELAY 0x02
#define TPS65132_REG_CFG   0x03
#define TPS65132_REG_CTL   0xFF

#define TPS65132_REG_CFG_APP_POS   6
#define TPS65132_REG_CFG_APP_MASK  BIT(6)
#define TPS65132_REG_CFG_SEQU_POS  4
#define TPS65132_REG_CFG_SEQU_MASK GENMASK(5, 4)
#define TPS65132_REG_CFG_SEQD_POS  2
#define TPS65132_REG_CFG_SEQD_MASK GENMASK(3, 2)
#define TPS65132_REG_CFG_DISP_MASK BIT(1)
#define TPS65132_REG_CFG_DISN_MASK BIT(0)

struct regulator_tps65132_desc {
	uint8_t vsel_reg;
	const struct linear_range *uv_range;
};

struct regulator_tps65132_common_config {
	struct i2c_dt_spec i2c;
	const struct device *vdd_reg;
	struct gpio_dt_spec enable;
	uint8_t apps;
	uint8_t sequ;
	uint8_t seqd;
};

struct regulator_tps65132_common_data {
	uint32_t enabled;    // at least one enable pin is active, then can read/write I2C
	bool hw_initialized; // hardware has been configured (lazy init)
};

struct regulator_tps65132_config {
	struct regulator_common_config common;
	struct gpio_dt_spec enable;
	const struct regulator_tps65132_desc *desc;
	const struct device *parent;
};

struct regulator_tps65132_data {
	struct regulator_common_data common;
	uint8_t vsel;
	bool hw_initialized;  // this channel has been configured (lazy init)
	uint8_t pending_vsel; // pending voltage setting to be applied on enable
	bool voltage_pending; // true if there's a pending voltage setting
};

/*
 * WARNING: Hardware Limitation
 *
 * Due to hardware design constraints, the maximum positive voltage
 * output of TPS65132 should not exceed 5.5V, and the negative voltage
 * should not exceed -4.5V. DO NOT MODIFY THE VOLTAGE RANGE SETTINGS
 * BELOW UNLESS YOU HAVE CONFIRMED HARDWARE COMPATIBILITY.
 */
static const struct linear_range vpos_buck_range = LINEAR_RANGE_INIT(4000000, 100000U, 0x0U, 0x0FU);
static const struct linear_range veng_buck_range = LINEAR_RANGE_INIT(4000000, 100000U, 0x0U, 0x05U);

static const struct regulator_tps65132_desc __maybe_unused vpos_desc = {
	.vsel_reg = TPS65132_REG_VPOS,
	.uv_range = &vpos_buck_range,
};

static const struct regulator_tps65132_desc __maybe_unused veng_desc = {
	.vsel_reg = TPS65132_REG_VENG,
	.uv_range = &veng_buck_range,
};

/* I2C retry configuration */
#define TPS65132_I2C_RETRY_COUNT    3
#define TPS65132_I2C_RETRY_DELAY_MS 5

/*
 * I2C wrapper functions with retry mechanism
 * These functions handle transient I2C errors that may occur after sleep/wakeup
 */
static int tps65132_i2c_read_byte(const struct i2c_dt_spec *i2c, uint8_t reg, uint8_t *val)
{
	int ret;
	int retries = TPS65132_I2C_RETRY_COUNT;

	do {
		ret = i2c_reg_read_byte_dt(i2c, reg, val);
		if (ret == 0) {
			return 0;
		}

		LOG_WRN("I2C read failed (reg=0x%02x, ret=%d), retries left: %d", reg, ret,
			retries - 1);

		if (--retries > 0) {
			k_sleep(K_MSEC(TPS65132_I2C_RETRY_DELAY_MS));
		}
	} while (retries > 0);

	LOG_ERR("I2C read failed after %d retries (reg=0x%02x)", TPS65132_I2C_RETRY_COUNT, reg);
	return ret;
}

static int tps65132_i2c_write_byte(const struct i2c_dt_spec *i2c, uint8_t reg, uint8_t val)
{
	int ret;
	int retries = TPS65132_I2C_RETRY_COUNT;

	do {
		ret = i2c_reg_write_byte_dt(i2c, reg, val);
		if (ret == 0) {
			return 0;
		}

		LOG_WRN("I2C write failed (reg=0x%02x, val=0x%02x, ret=%d), retries left: %d", reg,
			val, ret, retries - 1);

		if (--retries > 0) {
			k_sleep(K_MSEC(TPS65132_I2C_RETRY_DELAY_MS));
		}
	} while (retries > 0);

	LOG_ERR("I2C write failed after %d retries (reg=0x%02x)", TPS65132_I2C_RETRY_COUNT, reg);
	return ret;
}

static int tps65132_i2c_update_byte(const struct i2c_dt_spec *i2c, uint8_t reg, uint8_t mask,
				    uint8_t val)
{
	int ret;
	int retries = TPS65132_I2C_RETRY_COUNT;

	do {
		ret = i2c_reg_update_byte_dt(i2c, reg, mask, val);
		if (ret == 0) {
			return 0;
		}

		LOG_WRN("I2C update failed (reg=0x%02x, ret=%d), retries left: %d", reg, ret,
			retries - 1);

		if (--retries > 0) {
			k_sleep(K_MSEC(TPS65132_I2C_RETRY_DELAY_MS));
		}
	} while (retries > 0);

	LOG_ERR("I2C update failed after %d retries (reg=0x%02x)", TPS65132_I2C_RETRY_COUNT, reg);
	return ret;
}

/* Forward declaration */
static int regulator_tps65132_hw_init(const struct device *dev);

static int regulator_tps65132_common_enable(const struct device *dev)
{
	const struct regulator_tps65132_common_config *config = dev->config;
	struct regulator_tps65132_common_data *common_data = dev->data;
	int ret = 0;

	if (common_data->enabled != 0) {
		return 0;
	}

	// if the vdd regulator is not enabled, enable it first
	if (config->vdd_reg != NULL) {
		ret = regulator_enable(config->vdd_reg);
		if (ret < 0) {
			return ret;
		}
	}
	// set the configuration register
	if (config->enable.port != NULL) {
		ret = gpio_pin_set_dt(&config->enable, 1);
		if (ret < 0) {
			return ret;
		}
	}

	k_sleep(K_MSEC(50));

	return ret;
}

static int regulator_tps65132_common_disable(const struct device *dev)
{
	const struct regulator_tps65132_common_config *config = dev->config;
	struct regulator_tps65132_common_data *common_data = dev->data;
	int ret = 0;

	if (common_data->enabled != 0) {
		return 0;
	}

	// disable the enable pin
	if (config->enable.port != NULL) {
		ret = gpio_pin_set_dt(&config->enable, 0);
	}
	// if the vdd regulator is not enabled, enable it first
	if (config->vdd_reg != NULL) {
		ret = regulator_disable(config->vdd_reg);
	}
	return ret;
}

static int regulator_tps65132_enable(const struct device *dev)
{
	const struct regulator_tps65132_config *config = dev->config;
	const struct regulator_tps65132_common_config *common_config = config->parent->config;
	struct regulator_tps65132_common_data *common_data = config->parent->data;
	struct regulator_tps65132_data *data = dev->data;
	int ret = 0;

	/* Perform lazy initialization on first use */
	ret = regulator_tps65132_hw_init(dev);
	if (ret < 0) {
		LOG_ERR("TPS65132 hw init failed: %d", ret);
		return ret;
	}

	regulator_tps65132_common_enable(config->parent);

	if (config->enable.port != NULL) {
		ret = gpio_pin_set_dt(&config->enable, 1);
		if (ret < 0) {
			return ret;
		}
		k_sleep(K_MSEC(1));
	}

	/* Apply pending voltage setting if any */
	if (data->voltage_pending) {
		uint8_t current_vsel;

		/* Read current voltage setting */
		ret = tps65132_i2c_read_byte(&common_config->i2c, config->desc->vsel_reg,
					     &current_vsel);
		if (ret < 0) {
			LOG_ERR("Failed to read current voltage: %d", ret);
			goto done;
		}

		/* Only write if different from current setting */
		if (current_vsel != data->pending_vsel) {
			ret = tps65132_i2c_write_byte(&common_config->i2c, config->desc->vsel_reg,
						      data->pending_vsel);
			if (ret < 0) {
				LOG_ERR("Failed to write voltage setting: %d", ret);
				goto done;
			}

			/* Write to EEPROM to save the setting */
			ret = tps65132_i2c_write_byte(&common_config->i2c, TPS65132_REG_CTL, 0x80);
			if (ret < 0) {
				LOG_ERR("Failed to write EEPROM control: %d", ret);
				goto done;
			}

			k_sleep(K_MSEC(60)); /* Wait for EEPROM update */
			LOG_INF("TPS65132 voltage setting applied: %d", data->pending_vsel);
		}

		/* Update stored voltage and clear pending flag */
		data->vsel = data->pending_vsel;
		data->voltage_pending = false;
	}

done:
	common_data->enabled++;
	return ret;
}

static int regulator_tps65132_disable(const struct device *dev)
{
	const struct regulator_tps65132_config *config = dev->config;
	struct regulator_tps65132_common_data *common_data = config->parent->data;
	int ret = 0;

	if (config->enable.port != NULL) {
		ret = gpio_pin_set_dt(&config->enable, 0);
	}

	// decrement the enabled count
	common_data->enabled--;

	regulator_tps65132_common_disable(config->parent);

	return ret;
}

static unsigned int regulator_tps65132_count_voltages(const struct device *dev)
{
	const struct regulator_tps65132_config *config = dev->config;

	return linear_range_values_count(config->desc->uv_range);
}

static int regulator_tps65132_list_voltage(const struct device *dev, unsigned int idx,
					   int32_t *volt_uv)
{
	const struct regulator_tps65132_config *config = dev->config;

	return linear_range_get_value(config->desc->uv_range, idx, volt_uv);
}

static int regulator_tps65132_get_voltage(const struct device *dev, int32_t *volt_uv)
{
	const struct regulator_tps65132_config *config = dev->config;
	const struct regulator_tps65132_common_config *common_config = config->parent->config;
	uint8_t idx;
	int ret;

	ret = tps65132_i2c_read_byte(&common_config->i2c, config->desc->vsel_reg, &idx);
	if (ret < 0) {
		return ret;
	}

	return linear_range_get_value(config->desc->uv_range, idx, volt_uv);
}

static int regulator_tps65132_set_voltage(const struct device *dev, int32_t min_volt_uv,
					  int32_t max_volt_uv)
{
	const struct regulator_tps65132_config *config = dev->config;
	struct regulator_tps65132_data *data = dev->data;
	int ret;
	uint16_t idx;

	ret = linear_range_get_win_index(config->desc->uv_range, min_volt_uv, max_volt_uv, &idx);

	if (ret < 0) {
		return ret;
	}

	/* Store the desired voltage setting to be applied when enabled */
	data->pending_vsel = (uint8_t)idx;
	data->voltage_pending = true;

	LOG_INF("TPS65132 set_voltage min: %d, max: %d (pending until enable)", min_volt_uv,
		max_volt_uv);

	return 0;
}

static int regulator_tps65132_set_active_discharge(const struct device *dev, bool active_discharge)
{
	int ret = 0;
	const struct regulator_tps65132_config *config = dev->config;
	const struct regulator_tps65132_common_config *common_config = config->parent->config;
	struct regulator_tps65132_common_data *common_data = config->parent->data;
	uint8_t value = 0;

	if (common_data->enabled == 0) {
		regulator_tps65132_common_enable(config->parent);
		// must enable the enable pin before writing to the vsel reg
		if (config->enable.port != NULL) {
			ret = gpio_pin_set_dt(&config->enable, 1);
			if (ret < 0) {
				return ret;
			}
			k_sleep(K_MSEC(1));
		}
	}

	// readback the value of cfg reg
	ret = tps65132_i2c_read_byte(&common_config->i2c, TPS65132_REG_CFG, &value);
	if (ret < 0) {
		goto done;
	}

	if (config->desc->vsel_reg == TPS65132_REG_VPOS) {
		// if the value is the same, no need to update
		if ((value & TPS65132_REG_CFG_DISP_MASK) == active_discharge) {
			goto done;
		}
		ret = tps65132_i2c_update_byte(&common_config->i2c, TPS65132_REG_CFG,
					       TPS65132_REG_CFG_DISP_MASK,
					       active_discharge ? TPS65132_REG_CFG_DISP_MASK : 0);

	} else {
		if ((value & TPS65132_REG_CFG_DISN_MASK) == active_discharge) {
			goto done;
		}
		ret = tps65132_i2c_update_byte(&common_config->i2c, TPS65132_REG_CFG,
					       TPS65132_REG_CFG_DISN_MASK,
					       active_discharge ? TPS65132_REG_CFG_DISN_MASK : 0);
	}

	// write to eeprom ctl 0x80
	ret = tps65132_i2c_write_byte(&common_config->i2c, TPS65132_REG_CTL, 0x80);
	if (ret < 0) {
		goto done;
	}

	k_sleep(K_MSEC(60)); // wait 50ms for eeprom to update

done:
	if (common_data->enabled == 0) {
		if (config->enable.port != NULL) {
			gpio_pin_set_dt(&config->enable, 0);
		}
		regulator_tps65132_common_disable(config->parent);
	}
	return ret;
}

static int regulator_tps65132_get_active_discharge(const struct device *dev, bool *active_discharge)
{
	int ret = 0;
	const struct regulator_tps65132_config *config = dev->config;
	const struct regulator_tps65132_common_config *common_config = config->parent->config;
	struct regulator_tps65132_common_data *common_data = config->parent->data;
	uint8_t value = 0;

	if (common_data->enabled == 0) {
		regulator_tps65132_common_enable(config->parent);
		// must enable the enable pin before writing to the vsel reg
		if (config->enable.port != NULL) {
			ret = gpio_pin_set_dt(&config->enable, 1);
			if (ret < 0) {
				return ret;
			}
			k_sleep(K_MSEC(1));
		}
	}

	if (config->desc->vsel_reg == TPS65132_REG_VPOS) {
		ret = tps65132_i2c_read_byte(&common_config->i2c, config->desc->vsel_reg, &value);
		*active_discharge = value & TPS65132_REG_CFG_DISP_MASK;
	} else {
		ret = tps65132_i2c_read_byte(&common_config->i2c, config->desc->vsel_reg, &value);
		*active_discharge = value & TPS65132_REG_CFG_DISN_MASK;
	}

	if (common_data->enabled == 0) {
		if (config->enable.port != NULL) {
			gpio_pin_set_dt(&config->enable, 0);
		}
		regulator_tps65132_common_disable(config->parent);
	}

	return ret;
}

static int regulator_tps65132_get_current_limit(const struct device *dev, int32_t *curr_ua)
{
	const struct regulator_tps65132_config *config = dev->config;
	const struct regulator_tps65132_common_config *common_config = config->parent->config;
	struct regulator_tps65132_common_data *common_data = config->parent->data;

	// read apps
	uint8_t apps;
	int ret;

	if (common_data->enabled == 0) {
		regulator_tps65132_common_enable(config->parent);
		// must enable the enable pin before writing to the vsel reg
		if (config->enable.port != NULL) {
			ret = gpio_pin_set_dt(&config->enable, 1);
			if (ret < 0) {
				return ret;
			}
			k_sleep(K_MSEC(1));
		}
	}

	ret = tps65132_i2c_read_byte(&common_config->i2c, TPS65132_REG_CFG, &apps);
	if (ret < 0) {
		goto done;
	}
	*curr_ua = 40000 * (apps + 1);

done:
	if (common_data->enabled == 0) {
		if (config->enable.port != NULL) {
			gpio_pin_set_dt(&config->enable, 0);
		}
		regulator_tps65132_common_disable(config->parent);
	}

	return ret;
}

/**
 * @brief Perform lazy hardware initialization
 *
 * This function configures the hardware on first use to save power during boot.
 * It reads current settings, compares with desired configuration, and only
 * updates the chip if necessary (writing to EEPROM is expensive - 60ms).
 */
static int regulator_tps65132_hw_init(const struct device *dev)
{
	int ret;
	const struct regulator_tps65132_config *config = dev->config;
	const struct regulator_tps65132_common_config *common_config = config->parent->config;
	struct regulator_tps65132_common_data *common_data = config->parent->data;
	struct regulator_tps65132_data *data = dev->data;
	uint8_t value = 0;
	uint8_t cfg = 0;
	bool need_temp_enable = false;

	/* Already initialized */
	if (data->hw_initialized) {
		return 0;
	}

	LOG_DBG("TPS65132 lazy init for %s", dev->name);

	/* Temporarily enable if needed */
	if (common_data->enabled == 0) {
		need_temp_enable = true;
		regulator_tps65132_common_enable(config->parent);
		if (config->enable.port != NULL) {
			ret = gpio_pin_set_dt(&config->enable, 1);
			if (ret < 0) {
				return ret;
			}
		}
		k_sleep(K_MSEC(1));
	}

	/* Read current voltage setting */
	ret = tps65132_i2c_read_byte(&common_config->i2c, config->desc->vsel_reg, &data->vsel);
	if (ret < 0) {
		goto done;
	}

	/* Only configure common registers if not yet initialized */
	if (!common_data->hw_initialized) {
		ret = tps65132_i2c_read_byte(&common_config->i2c, TPS65132_REG_CFG, &value);
		if (ret < 0) {
			goto done;
		}

		cfg = common_config->apps << TPS65132_REG_CFG_APP_POS |
		      common_config->sequ << TPS65132_REG_CFG_SEQU_POS |
		      common_config->seqd << TPS65132_REG_CFG_SEQD_POS;

		/* Only update if configuration differs */
		if ((value & (TPS65132_REG_CFG_APP_MASK | TPS65132_REG_CFG_SEQU_MASK |
			      TPS65132_REG_CFG_SEQD_MASK)) != cfg) {

			cfg = (value & ~(TPS65132_REG_CFG_APP_MASK | TPS65132_REG_CFG_SEQU_MASK |
					 TPS65132_REG_CFG_SEQD_MASK)) |
			      cfg;

			ret = tps65132_i2c_update_byte(&common_config->i2c, TPS65132_REG_CFG, cfg,
						       cfg);
			if (ret < 0) {
				goto done;
			}

			/* Write to EEPROM (expensive operation) */
			ret = tps65132_i2c_write_byte(&common_config->i2c, TPS65132_REG_CTL, 0x80);
			if (ret < 0) {
				goto done;
			}

			k_sleep(K_MSEC(60)); /* Wait for EEPROM update */
		}

		common_data->hw_initialized = true;
	}

done:
	/* Disable if we temporarily enabled */
	if (need_temp_enable) {
		if (config->enable.port != NULL) {
			gpio_pin_set_dt(&config->enable, 0);
		}
		regulator_tps65132_common_disable(config->parent);
	}

	if (ret == 0) {
		data->hw_initialized = true;
		LOG_DBG("TPS65132 hw init done for %s", dev->name);
	}

	return ret;
}

static int regulator_tps65132_init(const struct device *dev)
{
	int ret;
	const struct regulator_tps65132_config *config = dev->config;
	struct regulator_tps65132_data *data = dev->data;

	regulator_common_data_init(dev);

	/* Verify parent device is ready */
	if (!device_is_ready(config->parent)) {
		LOG_ERR("TPS65132 parent device not ready");
		return -ENODEV;
	}

	/* Configure GPIO if present (but don't enable yet) */
	if (config->enable.port != NULL) {
		if (!gpio_is_ready_dt(&config->enable)) {
			LOG_ERR("TPS65132 GPIO not ready");
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(&config->enable, GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			LOG_ERR("TPS65132 GPIO configure failed: %d", ret);
			return ret;
		}
	}

	/* Mark as not initialized - will be done lazily on first use */
	data->hw_initialized = false;
	data->vsel = 0; /* Will be read on first access */
	data->pending_vsel = 0;
	data->voltage_pending = false;

	LOG_INF("TPS65132 %s init done (lazy mode)", dev->name);

	return regulator_common_init(dev, false);
}

static int regulator_tps65132_common_init(const struct device *dev)
{
	const struct regulator_tps65132_common_config *config = dev->config;
	struct regulator_tps65132_common_data *data = dev->data;

	if (!device_is_ready(config->i2c.bus)) {
		LOG_ERR("TPS65132 I2C bus not ready");
		return -ENODEV;
	}

	if (config->vdd_reg != NULL && !device_is_ready(config->vdd_reg)) {
		LOG_ERR("TPS65132 VDD regulator not ready");
		return -ENODEV;
	}

	if (config->enable.port != NULL) {
		if (!gpio_is_ready_dt(&config->enable)) {
			LOG_ERR("TPS65132 supply GPIO not ready");
			return -ENODEV;
		}
		int ret = gpio_pin_configure_dt(&config->enable, GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			LOG_ERR("TPS65132 supply GPIO configure failed: %d", ret);
			return ret;
		}
	}

	/* Mark as not initialized - will be configured lazily */
	data->hw_initialized = false;
	data->enabled = 0;

	LOG_INF("TPS65132 common init done (lazy mode)");
	return 0;
}

static const struct regulator_driver_api api = {
	.enable = regulator_tps65132_enable,
	.disable = regulator_tps65132_disable,
	.count_voltages = regulator_tps65132_count_voltages,
	.list_voltage = regulator_tps65132_list_voltage,
	.set_voltage = regulator_tps65132_set_voltage,
	.get_voltage = regulator_tps65132_get_voltage,
	.get_current_limit = regulator_tps65132_get_current_limit,
	.set_active_discharge = regulator_tps65132_set_active_discharge,
	.get_active_discharge = regulator_tps65132_get_active_discharge,
};

#define REGULATOR_TPS65132_DEFINE(node_id, id, name, _parent, prio)                                \
	static struct regulator_tps65132_data data_##id;                                           \
                                                                                                   \
	static const struct regulator_tps65132_config config_##id = {                              \
		.common = REGULATOR_DT_COMMON_CONFIG_INIT(node_id),                                \
		.enable = GPIO_DT_SPEC_GET_OR(node_id, enable_gpios, {0}),                         \
		.desc = &name##_desc,                                                              \
		.parent = _parent,                                                                 \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_DEFINE(node_id, regulator_tps65132_init, NULL, &data_##id, &config_##id,         \
			 POST_KERNEL, prio, &api);

#define REGULATOR_TPS65132_DEFINE_COND(inst, child, parent, prio)                                  \
	COND_CODE_1(DT_NODE_EXISTS(DT_INST_CHILD(inst, child)),                                    \
		    (REGULATOR_TPS65132_DEFINE(DT_INST_CHILD(inst, child), child##inst, child,     \
					       parent, prio)),                                     \
		    ())

#define REGULATOR_TPS65132_DEFINE_ALL(inst)                                                        \
	static const struct regulator_tps65132_common_config config_##inst = {                     \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.vdd_reg = DEVICE_DT_GET_OR_NULL(DT_INST_PHANDLE(inst, vin_supply)),               \
		.enable = GPIO_DT_SPEC_INST_GET_OR(inst, supply_gpios, {0}),                       \
		.apps = DT_INST_ENUM_IDX(inst, apps),                                              \
		.sequ = DT_INST_ENUM_IDX(inst, sequ),                                              \
		.seqd = DT_INST_ENUM_IDX(inst, seqd),                                              \
	};                                                                                         \
                                                                                                   \
	static struct regulator_tps65132_common_data data_##inst = {                               \
		.enabled = 0,                                                                      \
		.hw_initialized = false,                                                           \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, regulator_tps65132_common_init, NULL, &data_##inst,            \
			      &config_##inst, POST_KERNEL,                                         \
			      CONFIG_REGULATOR_TPS65132_COMMON_INIT_PRIORITY, NULL);               \
                                                                                                   \
	REGULATOR_TPS65132_DEFINE_COND(inst, vpos, DEVICE_DT_INST_GET(inst),                       \
				       CONFIG_REGULATOR_TPS65132_VPOS_INIT_PRIORITY)               \
	REGULATOR_TPS65132_DEFINE_COND(inst, veng, DEVICE_DT_INST_GET(inst),                       \
				       CONFIG_REGULATOR_TPS65132_VENG_INIT_PRIORITY)

DT_INST_FOREACH_STATUS_OKAY(REGULATOR_TPS65132_DEFINE_ALL)