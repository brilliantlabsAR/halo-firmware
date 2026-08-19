/*
 * Copyright (c) 2025 Bosch Sensortec GmbH
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT bosch_bma580

#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/policy.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/logging/log.h>

#include <drivers/bma580_sensor.h>

#include "bma5.h"
#include "bma580.h"
#include "bma580_features.h"

LOG_MODULE_REGISTER(BMA580, CONFIG_SENSOR_LOG_LEVEL);

/*!
 * @brief I2C read function adapted for Zephyr
 */
BMA5_INTF_RET_TYPE bma5_i2c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t len, void *intf_ptr)
{
	const struct i2c_dt_spec *i2c = (const struct i2c_dt_spec *)intf_ptr;
	int ret = i2c_write_read_dt(i2c, &reg_addr, 1, reg_data, len);
	return (ret == 0) ? BMA5_INTF_RET_SUCCESS : BMA5_INTF_RET_SUCCESS - 1;
}

/*!
 * @brief I2C write function adapted for Zephyr
 */
BMA5_INTF_RET_TYPE bma5_i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t len, void *intf_ptr)
{
	const struct i2c_dt_spec *i2c = (const struct i2c_dt_spec *)intf_ptr;
	uint8_t buf[17]; /* Max 16 bytes + reg */
	buf[0] = reg_addr;
	memcpy(&buf[1], reg_data, len);
	int ret = i2c_write_dt(i2c, buf, len + 1);
	return (ret == 0) ? BMA5_INTF_RET_SUCCESS : BMA5_INTF_RET_SUCCESS - 1;
}

/*!
 * @brief Delay function adapted for Zephyr
 */
void bma5_delay_us(uint32_t period_us, void *intf_ptr)
{
	k_usleep(period_us);
}

static int bma580_do_init(const struct device *dev);

/* Configuration Parameters */
#define BMA580_CHIP_ID_EXPECTED 0xC4

struct bma580_config {
	struct i2c_dt_spec i2c;
#ifdef CONFIG_BMA580_TRIGGER
	struct gpio_dt_spec int1_gpio;
	struct gpio_dt_spec int2_gpio;
	uint8_t int1_conf;
	uint8_t int2_conf;
#endif
	const struct device *vdd_reg;
	uint8_t range;
	uint8_t odr;
	bool lazy_init;
};

struct bma580_data {
	struct bma5_dev bma5_dev;
	int16_t accel_x;
	int16_t accel_y;
	int16_t accel_z;
	int8_t temp;
	uint8_t odr;
	uint8_t range;
	bool is_initialized;
#ifdef CONFIG_BMA580_TRIGGER
	const struct device *dev;
	struct gpio_callback gpio_cb;

	const struct sensor_trigger *data_ready_trigger;
	sensor_trigger_handler_t data_ready_handler;

	const struct sensor_trigger *tap_trigger;
	sensor_trigger_handler_t tap_handler;

	const struct sensor_trigger *double_tap_trigger;
	sensor_trigger_handler_t double_tap_handler;

	const struct sensor_trigger *triple_tap_trigger;
	sensor_trigger_handler_t triple_tap_handler;

	/* Tap-detector configuration applied whenever a tap trigger is
	 * (re)armed. Defaults set in bma580_sensor_init(); tunable at runtime
	 * via the BMA580_ATTR_TAP_* attributes. The s/d/t_tap_en fields are
	 * ignored here — enables are derived from the registered handlers. */
	struct bma580_tap_config tap_cfg;

#if defined(CONFIG_BMA580_TRIGGER_OWN_THREAD)
	K_KERNEL_STACK_MEMBER(thread_stack, CONFIG_BMA580_THREAD_STACK_SIZE);
	struct k_thread thread;
	struct k_sem gpio_sem;
#elif defined(CONFIG_BMA580_TRIGGER_GLOBAL_THREAD)
	struct k_work work;
#endif
#endif
};

#ifdef CONFIG_BMA580_TRIGGER
static inline void setup_int(const struct device *dev, bool enable)
{
	const struct bma580_config *config = dev->config;

	if (config->int1_gpio.port) {
		gpio_pin_interrupt_configure_dt(
			&config->int1_gpio, (enable ? GPIO_INT_EDGE_TO_ACTIVE : GPIO_INT_DISABLE));
	}
	if (config->int2_gpio.port) {
		gpio_pin_interrupt_configure_dt(
			&config->int2_gpio, (enable ? GPIO_INT_EDGE_TO_ACTIVE : GPIO_INT_DISABLE));
	}
}

static void bma580_gpio_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	struct bma580_data *data = CONTAINER_OF(cb, struct bma580_data, gpio_cb);

	ARG_UNUSED(pins);

	setup_int(data->dev, false);

#if defined(CONFIG_BMA580_TRIGGER_OWN_THREAD)
	k_sem_give(&data->gpio_sem);
#elif defined(CONFIG_BMA580_TRIGGER_GLOBAL_THREAD)
	k_work_submit(&data->work);
#endif
}

static void bma580_thread_cb(const struct device *dev)
{
	struct bma580_data *data = dev->data;
	struct bma580_int_status_types int_status = {0};
	int err = 0;

	int_status.int_src = BMA580_INT_STATUS_INT1;

	/* read interrupt status */
	err = bma580_get_int_status(&int_status, 1, &data->bma5_dev);
	if (err < 0) {
		LOG_ERR("Failed to read INT1 status");
		return;
	}

	if (int_status.int_status.acc_drdy_int_status) {
		if (data->data_ready_handler) {
			data->data_ready_handler(dev, data->data_ready_trigger);
		}
	}
	
	if (int_status.int_status.stap_int_status) {
		if (data->tap_handler) {
			data->tap_handler(dev, data->tap_trigger);
		}
	}

	if (int_status.int_status.dtap_int_status) {
		if (data->double_tap_handler) {
			data->double_tap_handler(dev, data->double_tap_trigger);
		}
	}

	if (int_status.int_status.ttap_int_status) {
		if (data->triple_tap_handler) {
			data->triple_tap_handler(dev, data->triple_tap_trigger);
		}
	}

	/* Clear interrupt status */
	int_status.int_status.acc_drdy_int_status = 1;
	int_status.int_status.stap_int_status = 1;
	int_status.int_status.dtap_int_status = 1;
	int_status.int_status.ttap_int_status = 1;

	err = bma580_set_int_status(&int_status, 1, &data->bma5_dev);
	if (err < 0) {
		LOG_ERR("Failed to clear INT1 status");
		return;
	}

	setup_int(dev, true);
}

#ifdef CONFIG_BMA580_TRIGGER_OWN_THREAD
static void bma580_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct bma580_data *data = p1;

	while (1) {
		k_sem_take(&data->gpio_sem, K_FOREVER);
		bma580_thread_cb(data->dev);
	}
}
#endif

#ifdef CONFIG_BMA580_TRIGGER_GLOBAL_THREAD
static void bma580_work_cb(struct k_work *work)
{
	struct bma580_data *data = CONTAINER_OF(work, struct bma580_data, work);

	bma580_thread_cb(data->dev);
}
#endif

#endif

static int bma580_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	struct bma580_data *data = dev->data;
	uint8_t buf[6];
	int ret;

	// Read accelerometer data
	ret = bma5_get_regs(BMA5_REG_ACC_DATA_0, buf, sizeof(buf), &data->bma5_dev);
	if (ret < 0) {
		LOG_ERR("Failed to read acceleration data: %d", ret);
		return ret;
	}

	data->accel_x = (buf[1] << 8) | buf[0];
	data->accel_y = (buf[3] << 8) | buf[2];
	data->accel_z = (buf[5] << 8) | buf[4];

	// Read temperature data
	ret = bma5_get_regs(BMA5_REG_TEMP_DATA, &buf[0], 1, &data->bma5_dev);
	if (ret < 0) {
		LOG_ERR("Failed to read temperature data");
		return ret;
	}
	data->temp = buf[0];

	return 0;
}

static int bma580_channel_get(const struct device *dev, enum sensor_channel chan,
			      struct sensor_value *val)
{
	struct bma580_data *data = dev->data;

	double lsb_to_g;

	/* Determine scale factor based on range */
	switch (data->range) {
	case BMA5_ACC_RANGE_MAX_2G: // 2G
		lsb_to_g = 16384.0f;
		break;
	case BMA5_ACC_RANGE_MAX_4G: // 4G
		lsb_to_g = 8192.0f;
		break;
	case BMA5_ACC_RANGE_MAX_8G: // 8G
		lsb_to_g = 4096.0f;
		break;
	case BMA5_ACC_RANGE_MAX_16G: // 16G
		lsb_to_g = 2048.0f;
		break;
	default:
		return -EINVAL;
	}

	switch (chan) {
	case SENSOR_CHAN_ACCEL_X:
		sensor_value_from_double(val, data->accel_x / lsb_to_g);
		break;
	case SENSOR_CHAN_ACCEL_Y:
		sensor_value_from_double(val, data->accel_y / lsb_to_g);
		break;
	case SENSOR_CHAN_ACCEL_Z:
		sensor_value_from_double(val, data->accel_z / lsb_to_g);
		break;
	case SENSOR_CHAN_ACCEL_XYZ:
		sensor_value_from_double(val, data->accel_x / lsb_to_g);
		sensor_value_from_double(val + 1, data->accel_y / lsb_to_g);
		sensor_value_from_double(val + 2, data->accel_z / lsb_to_g);
		break;
	case SENSOR_CHAN_DIE_TEMP:
		val->val1 = (int8_t)(data->temp + 23);
	default:
		return -ENOTSUP;
	}

	return 0;
}

#ifdef CONFIG_BMA580_TRIGGER
/**
 * @brief Write the stored tap configuration to the feature engine.
 *
 * Gesture enables are derived from the registered trigger handlers; the
 * per-gesture interrupts are mapped to INT1 accordingly. Called whenever a
 * tap trigger is (re)armed or a tap attribute changes while armed.
 */
static int bma580_apply_tap_config(const struct device *dev)
{
	struct bma580_data *data = dev->data;
	struct bma580_tap_config tap_config = data->tap_cfg;
	int ret;

	tap_config.s_tap_en = (data->tap_handler != NULL) ? 1 : 0;
	tap_config.d_tap_en = (data->double_tap_handler != NULL) ? 1 : 0;
	tap_config.t_tap_en = (data->triple_tap_handler != NULL) ? 1 : 0;

	ret = bma580_set_tap_config(&tap_config, &data->bma5_dev);
	if (ret != BMA5_OK) {
		LOG_ERR("Failed to set tap config: %d", ret);
		return -EIO;
	}

	// Enable/disable the tap feature in GPR_0
	struct bma580_feat_eng_gpr_0 gpr_0;
	ret = bma580_get_feat_eng_gpr_0(&gpr_0, &data->bma5_dev);
	if (ret != BMA5_OK) {
		LOG_ERR("Failed to get GPR_0: %d", ret);
		return -EIO;
	}

	gpr_0.tap_en = (tap_config.s_tap_en || tap_config.d_tap_en || tap_config.t_tap_en)
			       ? BMA5_ENABLE
			       : BMA5_DISABLE;

	ret = bma580_set_feat_eng_gpr_0(&gpr_0, &data->bma5_dev);
	if (ret != BMA5_OK) {
		LOG_ERR("Failed to set GPR_0: %d", ret);
		return -EIO;
	}

	// Set GPR control to enable host access
	uint8_t gpr_ctrl_host = BMA5_ENABLE;
	ret = bma5_set_regs(BMA5_REG_FEAT_ENG_GPR_CTRL, &gpr_ctrl_host, 1, &data->bma5_dev);
	if (ret != BMA5_OK) {
		LOG_ERR("Failed to set GPR control: %d", ret);
		return -EIO;
	}

	// Map each enabled tap interrupt to INT1
	struct bma580_int_map int_map;
	ret = bma580_get_int_map(&int_map, &data->bma5_dev);
	if (ret != BMA5_OK) {
		LOG_ERR("Failed to get interrupt map: %d", ret);
		return -EIO;
	}

	int_map.stap_int_map =
		tap_config.s_tap_en ? BMA580_STAP_INT_MAP_INT1 : BMA580_STAP_INT_MAP_UNMAPPED;
	int_map.dtap_int_map =
		tap_config.d_tap_en ? BMA580_DTAP_INT_MAP_INT1 : BMA580_DTAP_INT_MAP_UNMAPPED;
	int_map.ttap_int_map =
		tap_config.t_tap_en ? BMA580_TTAP_INT_MAP_INT1 : BMA580_TTAP_INT_MAP_UNMAPPED;

	ret = bma580_set_int_map(&int_map, &data->bma5_dev);
	if (ret != BMA5_OK) {
		LOG_ERR("Failed to map tap interrupts: %d", ret);
		return -EIO;
	}

	return 0;
}

static bool bma580_any_tap_handler(const struct bma580_data *data)
{
	return data->tap_handler || data->double_tap_handler || data->triple_tap_handler;
}

/**
 * @brief Set one BMA580_ATTR_TAP_* attribute.
 *
 * The value is stored in the driver and, if a tap trigger is currently
 * armed, written through to the feature engine immediately. Values are raw
 * chip units, range-checked against the field widths in bma580_features.h.
 */
static int bma580_tap_attr_set(const struct device *dev, enum sensor_attribute attr,
			       const struct sensor_value *val)
{
	struct bma580_data *data = dev->data;
	int v = val->val1;

	switch ((int)attr) {
	case BMA580_ATTR_TAP_MODE:
		if (v < BMA580_TAP_MODE_SENSITIVE || v > BMA580_TAP_MODE_ROBUST) {
			return -EINVAL;
		}
		data->tap_cfg.mode = v;
		break;
	case BMA580_ATTR_TAP_AXIS:
		/* 2-bit field; 3 is reserved */
		if (v < BMA580_TAP_SEL_AXIS_X || v > BMA580_TAP_SEL_AXIS_Z) {
			return -EINVAL;
		}
		data->tap_cfg.axis_sel = v;
		break;
	case BMA580_ATTR_TAP_PEAK_THRES:
		if (v < 0 || v > (BMA580_TAP_PEAK_THRES_MSK >> BMA580_TAP_PEAK_THRES_POS)) {
			return -EINVAL;
		}
		data->tap_cfg.tap_peak_thres = v;
		break;
	case BMA580_ATTR_TAP_MAX_PEAKS:
		if (v < 0 || v > (BMA580_TAP_MAX_PEAKS_FOR_TAP_MSK >> BMA580_TAP_MAX_PEAKS_FOR_TAP_POS)) {
			return -EINVAL;
		}
		data->tap_cfg.max_peaks_for_tap = v;
		break;
	case BMA580_ATTR_TAP_MAX_GESTURE_DUR:
		if (v < 0 || v > (BMA580_TAP_MAX_GESTURE_DUR_MSK >> BMA580_TAP_MAX_GESTURE_DUR_POS)) {
			return -EINVAL;
		}
		data->tap_cfg.max_gesture_dur = v;
		break;
	case BMA580_ATTR_TAP_WAIT_FOR_TIMEOUT:
		if (v < 0 || v > 1) {
			return -EINVAL;
		}
		data->tap_cfg.wait_for_timeout = v;
		break;
	case BMA580_ATTR_TAP_MAX_DUR_BETWEEN_PEAKS:
		if (v < 0 || v > (BMA580_TAP_MAX_DUR_BET_PEAKS_MSK >> BMA580_TAP_MAX_DUR_BET_PEAKS_POS)) {
			return -EINVAL;
		}
		data->tap_cfg.max_dur_between_peaks = v;
		break;
	case BMA580_ATTR_TAP_SHOCK_SETTLING_DUR:
		if (v < 0 || v > (BMA580_TAP_SHOCK_DET_DUR_MSK >> BMA580_TAP_SHOCK_DET_DUR_POS)) {
			return -EINVAL;
		}
		data->tap_cfg.tap_shock_settling_dur = v;
		break;
	case BMA580_ATTR_TAP_MIN_QUIET_DUR:
		if (v < 0 || v > (BMA580_TAP_MIN_QUITE_DUR_MSK >> BMA580_TAP_MIN_QUITE_DUR_POS)) {
			return -EINVAL;
		}
		data->tap_cfg.min_quite_dur_between_taps = v;
		break;
	case BMA580_ATTR_TAP_QUIET_TIME_AFTER_GESTURE:
		if (v < 0 || v > (BMA580_TAP_QUITE_TIME_MSK >> BMA580_TAP_QUITE_TIME_POS)) {
			return -EINVAL;
		}
		data->tap_cfg.quite_time_after_gesture = v;
		break;
	default:
		return -ENOTSUP;
	}

	/* Write through if the tap engine is currently armed. */
	if (data->is_initialized && bma580_any_tap_handler(data)) {
		return bma580_apply_tap_config(dev);
	}

	return 0;
}

static int bma580_attr_get(const struct device *dev, enum sensor_channel chan,
			   enum sensor_attribute attr, struct sensor_value *val)
{
	struct bma580_data *data = dev->data;

	ARG_UNUSED(chan);

	val->val2 = 0;

	switch ((int)attr) {
	case BMA580_ATTR_TAP_MODE:
		val->val1 = data->tap_cfg.mode;
		break;
	case BMA580_ATTR_TAP_AXIS:
		val->val1 = data->tap_cfg.axis_sel;
		break;
	case BMA580_ATTR_TAP_PEAK_THRES:
		val->val1 = data->tap_cfg.tap_peak_thres;
		break;
	case BMA580_ATTR_TAP_MAX_PEAKS:
		val->val1 = data->tap_cfg.max_peaks_for_tap;
		break;
	case BMA580_ATTR_TAP_MAX_GESTURE_DUR:
		val->val1 = data->tap_cfg.max_gesture_dur;
		break;
	case BMA580_ATTR_TAP_WAIT_FOR_TIMEOUT:
		val->val1 = data->tap_cfg.wait_for_timeout;
		break;
	case BMA580_ATTR_TAP_MAX_DUR_BETWEEN_PEAKS:
		val->val1 = data->tap_cfg.max_dur_between_peaks;
		break;
	case BMA580_ATTR_TAP_SHOCK_SETTLING_DUR:
		val->val1 = data->tap_cfg.tap_shock_settling_dur;
		break;
	case BMA580_ATTR_TAP_MIN_QUIET_DUR:
		val->val1 = data->tap_cfg.min_quite_dur_between_taps;
		break;
	case BMA580_ATTR_TAP_QUIET_TIME_AFTER_GESTURE:
		val->val1 = data->tap_cfg.quite_time_after_gesture;
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}
#endif /* CONFIG_BMA580_TRIGGER */

static int bma580_attr_set(const struct device *dev, enum sensor_channel chan,
			   enum sensor_attribute attr, const struct sensor_value *val)
{
	struct bma580_data *data = dev->data;
	int ret;

#ifdef CONFIG_BMA580_TRIGGER
	if ((int)attr >= SENSOR_ATTR_PRIV_START) {
		return bma580_tap_attr_set(dev, attr, val);
	}
#endif

	switch (attr) {
	case SENSOR_ATTR_SAMPLING_FREQUENCY:
		if (chan != SENSOR_CHAN_ACCEL_XYZ) {
			return -ENOTSUP;
		}
		// Map frequency to Bosch ODR
		{
			int freq = val->val1;
			uint8_t odr;
			switch (freq) {
			case 1: odr = BMA5_ACC_ODR_HZ_1P5625; break; // approx 1.56
			case 3: odr = BMA5_ACC_ODR_HZ_3P125; break; // approx 3.125
			case 6: odr = BMA5_ACC_ODR_HZ_6P25; break; // approx 6.25
			case 12: odr = BMA5_ACC_ODR_HZ_12P5; break;
			case 25: odr = BMA5_ACC_ODR_HZ_25; break;
			case 50: odr = BMA5_ACC_ODR_HZ_50; break;
			case 100: odr = BMA5_ACC_ODR_HZ_100; break;
			case 200: odr = BMA5_ACC_ODR_HZ_200; break;
			case 400: odr = BMA5_ACC_ODR_HZ_400; break;
			case 800: odr = BMA5_ACC_ODR_HZ_800; break;
			case 1600: odr = BMA5_ACC_ODR_HZ_1K6; break;
			default: return -EINVAL;
			}
			data->odr = odr;
		}
		break;
	case SENSOR_ATTR_FULL_SCALE:
		if (chan != SENSOR_CHAN_ACCEL_XYZ) {
			return -ENOTSUP;
		}
		// Map scale to Bosch range
		{
			int scale = val->val1;
			uint8_t range;
			switch (scale) {
			case 2: range = BMA5_ACC_RANGE_MAX_2G; break;
			case 4: range = BMA5_ACC_RANGE_MAX_4G; break;
			case 8: range = BMA5_ACC_RANGE_MAX_8G; break;
			case 16: range = BMA5_ACC_RANGE_MAX_16G; break;
			default: return -EINVAL;
			}
			data->range = range;
		}
		break;
	default:
		return -ENOTSUP;
	}

	// Apply the configuration
	struct bma5_acc_conf acc_conf = {0};
	acc_conf.acc_odr = data->odr;
	acc_conf.acc_range = data->range;
	acc_conf.power_mode = BMA5_POWER_MODE_HPM; // High Performance Mode
	acc_conf.acc_bwp = BMA5_ACC_BWP_NORM_AVG4; // Normal bandwidth
	acc_conf.noise_mode = BMA5_NOISE_MODE_LOWER_NOISE; // Lower noise

	ret = bma5_set_acc_conf(&acc_conf, &data->bma5_dev);
	if (ret != BMA5_OK) {
		LOG_ERR("Failed to set accelerometer configuration: %d", ret);
		return -EIO;
	}

	return 0;
}

#ifdef CONFIG_BMA580_TRIGGER
int bma580_trigger_set(const struct device *dev, const struct sensor_trigger *trig,
		       sensor_trigger_handler_t handler)
{
	struct bma580_data *data = dev->data;
	const struct bma580_config *config = dev->config;
	int ret;

	if (!config->int1_gpio.port && !config->int2_gpio.port) {
		return -ENOTSUP;
	}

	if (trig->type == SENSOR_TRIG_DATA_READY) {
		if (handler == NULL) {
			return -EINVAL;
		}
		data->data_ready_trigger = trig;
		data->data_ready_handler = handler;

		// Configure data ready interrupt mapping
		struct bma580_int_map int_map;
		ret = bma580_get_int_map(&int_map, &data->bma5_dev);
		if (ret != BMA5_OK) {
			LOG_ERR("Failed to get interrupt map: %d", ret);
			return -EIO;
		}

		// Map data ready interrupt to INT1
		int_map.acc_drdy_int_map = BMA580_ACC_DRDY_INT_MAP_INT1;

		ret = bma580_set_int_map(&int_map, &data->bma5_dev);
		if (ret != BMA5_OK) {
			LOG_ERR("Failed to map data ready interrupt: %d", ret);
			return -EIO;
		}

		return 0;
	}

	/* Tap triggers: a NULL handler disarms that gesture. The tap engine
	 * watches a single dominant axis; an explicit accel channel in the
	 * trigger overrides the configured BMA580_ATTR_TAP_AXIS. */
	switch ((int)trig->type) {
	case SENSOR_TRIG_TAP:
		data->tap_trigger = handler ? trig : NULL;
		data->tap_handler = handler;
		break;
	case SENSOR_TRIG_DOUBLE_TAP:
		data->double_tap_trigger = handler ? trig : NULL;
		data->double_tap_handler = handler;
		break;
	case BMA580_TRIG_TRIPLE_TAP:
		data->triple_tap_trigger = handler ? trig : NULL;
		data->triple_tap_handler = handler;
		break;
	default:
		return -ENOTSUP;
	}

	switch (trig->chan) {
	case SENSOR_CHAN_ACCEL_X:
		data->tap_cfg.axis_sel = BMA580_TAP_SEL_AXIS_X;
		break;
	case SENSOR_CHAN_ACCEL_Y:
		data->tap_cfg.axis_sel = BMA580_TAP_SEL_AXIS_Y;
		break;
	case SENSOR_CHAN_ACCEL_Z:
		data->tap_cfg.axis_sel = BMA580_TAP_SEL_AXIS_Z;
		break;
	default:
		/* SENSOR_CHAN_ACCEL_XYZ etc: keep the configured axis */
		break;
	}

	return bma580_apply_tap_config(dev);
}

int bma580_init_interrupt(const struct device *dev)
{
	struct bma580_data *data = dev->data;
	const struct bma580_config *config = dev->config;
	int err = 0;

	/* setup data ready gpio interrupt */
	if (config->int1_gpio.port != NULL) {
		if (!gpio_is_ready_dt(&config->int1_gpio)) {
			LOG_ERR("GPIO device not ready");
			return -ENODEV;
		}

		gpio_pin_configure_dt(&config->int1_gpio, GPIO_INPUT);

		gpio_init_callback(&data->gpio_cb, bma580_gpio_callback,
				   BIT(config->int1_gpio.pin));

		if (gpio_add_callback(config->int1_gpio.port, &data->gpio_cb) < 0) {
			LOG_DBG("Could not set gpio callback");
			return -EIO;
		}

		// Configure interrupt
		struct bma5_int_conf int_conf;
		int_conf.int_mode = BMA5_INT1_MODE_LATCHED;
		int_conf.int_od = BMA5_INT1_OD_PUSH_PULL;
		int_conf.int_lvl = BMA5_INT1_LVL_ACTIVE_HIGH;

		struct bma5_int_conf_types int_config;
		int_config.int_src = BMA5_INT_1;
		int_config.int_conf = int_conf;

		err = bma5_set_int_conf(&int_config, 1, &data->bma5_dev);
		if (err < 0) {
			LOG_ERR("Failed to write INT1_CONF");
			return err;
		}
	}

	data->dev = dev;

#if defined(CONFIG_BMA580_TRIGGER_OWN_THREAD)
	k_sem_init(&data->gpio_sem, 0, K_SEM_MAX_LIMIT);

	k_thread_create(&data->thread, data->thread_stack, CONFIG_BMA580_THREAD_STACK_SIZE,
			bma580_thread, data, NULL, NULL, K_PRIO_COOP(CONFIG_BMA580_THREAD_PRIORITY),
			0, K_NO_WAIT);
#elif defined(CONFIG_BMA580_TRIGGER_GLOBAL_THREAD)
	data->work.handler = bma580_work_cb;
#endif

	setup_int(dev, true);

	return 0;
}

#endif

static int bma580_do_init(const struct device *dev)
{
	const struct bma580_config *cfg = dev->config;
	struct bma580_data *data = dev->data;
	uint8_t chip_id;
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

	// Wait for power to stabilize - reduced from 20ms to 10ms for faster boot
	k_sleep(K_MSEC(10));

	// Initialize BMA5 device structure manually
	data->bma5_dev.bus_read = bma5_i2c_read;
	data->bma5_dev.bus_write = bma5_i2c_write;
	data->bma5_dev.intf = BMA5_I2C_INTF;
	data->bma5_dev.intf_ptr = (void *)&cfg->i2c;
	data->bma5_dev.delay_us = bma5_delay_us;
	data->bma5_dev.context = BMA5_HEARABLE;
	data->bma5_dev.dummy_byte = 0;

	// Initialize BMA580 device
	ret = bma580_init(&data->bma5_dev);
	if (ret != BMA5_OK) {
		LOG_ERR("Failed to initialize BMA580");
		return -EIO;
	}

	/* Verify chip ID */
	ret = bma580_get_chip_id(&chip_id, &data->bma5_dev);
	if (ret < 0) {
		LOG_ERR("Failed to read chip ID: %d", ret);
		return ret;
	}

	if (chip_id != BMA580_CHIP_ID_EXPECTED) {
		LOG_ERR("Invalid chip ID 0x%02x", chip_id);
		return -EINVAL;
	}

	// Set initial ODR and range from config
	data->odr = cfg->odr;
	data->range = cfg->range;

	// Configure accelerometer
	struct bma5_acc_conf acc_conf = {0};
	acc_conf.acc_odr = data->odr;
	acc_conf.acc_range = data->range;
	acc_conf.power_mode = BMA5_POWER_MODE_HPM; // High Performance Mode
	acc_conf.acc_bwp = BMA5_ACC_BWP_NORM_AVG4; // Normal bandwidth
	acc_conf.noise_mode = BMA5_NOISE_MODE_LOWER_NOISE; // Lower noise

	ret = bma5_set_acc_conf(&acc_conf, &data->bma5_dev);
	if (ret != BMA5_OK) {
		LOG_ERR("Failed to set accelerometer configuration");
		return -EIO;
	}

#ifdef CONFIG_BMA580_TRIGGER
	if (cfg->int1_gpio.port || cfg->int2_gpio.port) {
		if (bma580_init_interrupt(dev) < 0) {
			LOG_DBG("Could not initialize interrupts");
			return -EIO;
		}
	}
#endif

	data->is_initialized = true;

	LOG_INF("BMA580 initialized successfully");

	return 0;
}

static int bma580_sensor_init(const struct device *dev)
{
	const struct bma580_config *cfg = dev->config;
	struct bma580_data *data = dev->data;

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

#ifdef CONFIG_BMA580_TRIGGER
	/* Tap-detector defaults, applied when a tap trigger is armed and
	 * tunable at runtime via the BMA580_ATTR_TAP_* attributes. Tuned on
	 * a worn production Halo (2026-08-14): NORMAL/200 on the Z axis
	 * (temple-tap axis; the field selects ONE dominant axis — gravity
	 * sits on X worn) gives 3/3 worn single taps and rejects ordinary
	 * table set-downs; the old SENSITIVE/24 read one tap's ringing as a
	 * double. Quiet/peak timings are the Bosch hearable-context values. */
	data->tap_cfg = (struct bma580_tap_config){
		.axis_sel = BMA580_TAP_SEL_AXIS_Z,
		.wait_for_timeout = 0x1,
		.max_peaks_for_tap = 0x6,
		.mode = BMA580_TAP_MODE_NORMAL,
		.tap_peak_thres = 200,
		.max_gesture_dur = 30,
		.max_dur_between_peaks = 0x4,
		.tap_shock_settling_dur = 0x6,
		.min_quite_dur_between_taps = 0x8,
		.quite_time_after_gesture = 0x6,
	};
#endif

#ifdef CONFIG_PM_DEVICE
	pm_device_init_suspended(dev);
#endif
	
	if (cfg->lazy_init) {
		LOG_INF("BMA580 init done (lazy-init mode: hardware init deferred until resume)");
	} else {
		LOG_INF("BMA580 init done (normal mode: hardware init deferred until resume)");
	}

	return 0;
}
static int bma580_power_off(const struct device *dev)
{
	const struct bma580_config *cfg = dev->config;
	int ret = 0;
	if (cfg->vdd_reg) {
		ret = regulator_disable(cfg->vdd_reg);
	}
	return ret;
}

static int bma580_pm_action(const struct device *dev, enum pm_device_action action)
{
	int ret = 0;
	struct bma580_data *data = dev->data;
	
	pm_device_state_lock(dev);
	switch (action) {
	case PM_DEVICE_ACTION_SUSPEND:
		if (data->is_initialized) {
			data->is_initialized = false; /* Will re-init on resume */
		}
		break;
	case PM_DEVICE_ACTION_RESUME:
		if (!data->is_initialized) {
			ret = bma580_do_init(dev);
			if (ret != 0) {
				LOG_ERR("Hardware init failed on resume: %d", ret);
				pm_device_state_unlock(dev);
				return ret;
			}
		}
		break;
	case PM_DEVICE_ACTION_TURN_ON:
		ret = bma580_do_init(dev);
		if (ret < 0) {
			LOG_ERR("Failed to turn on device");
		}
		break;
	case PM_DEVICE_ACTION_TURN_OFF:
		ret = bma580_power_off(dev);
		if (ret < 0) {
			LOG_ERR("Failed to turn off device");
		}
		break;
	default:
		ret = -ENOTSUP;
		break;
	}
	pm_device_state_unlock(dev);
	return ret;
}

static const struct sensor_driver_api bma580_api = {
	.attr_set = bma580_attr_set,
#ifdef CONFIG_BMA580_TRIGGER
	.attr_get = bma580_attr_get,
	.trigger_set = bma580_trigger_set,
#endif
	.sample_fetch = bma580_sample_fetch,
	.channel_get = bma580_channel_get,
};

#define BMA580_DEFINE(inst)                                                                        \
	static struct bma580_data bma580_data_##inst;                                              \
	static const struct bma580_config bma580_config_##inst = {                                 \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.vdd_reg = DEVICE_DT_GET_OR_NULL(DT_INST_PHANDLE(inst, vin_supply)),               \
		.range = DT_INST_ENUM_IDX(inst, range),                                            \
		.odr = DT_INST_ENUM_IDX(inst, odr),                                                \
		.lazy_init = DT_INST_PROP_OR(inst, lazy_init, false),                                        \
		IF_ENABLED(CONFIG_BMA580_TRIGGER,                                                  \
			   (.int1_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, int1_gpios, {0}),          \
			    .int2_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, int2_gpios, {0}),          \
			    .int1_conf = DT_INST_PROP(inst, int1_config),                          \
			    .int2_conf = DT_INST_PROP(inst, int2_config), ))};                     \
	PM_DEVICE_DT_INST_DEFINE(inst, bma580_pm_action);                                          \
	DEVICE_DT_INST_DEFINE(inst, &bma580_sensor_init, PM_DEVICE_DT_INST_GET(inst),                     \
			      &bma580_data_##inst, &bma580_config_##inst, POST_KERNEL,             \
			      CONFIG_SENSOR_INIT_PRIORITY, &bma580_api);

DT_INST_FOREACH_STATUS_OKAY(BMA580_DEFINE)