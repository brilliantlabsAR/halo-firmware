/*
 * Copyright (c) 2025 Alif Semiconductor
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT alif_alif_vbat

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/pm/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <math.h>
#include <string.h>

LOG_MODULE_REGISTER(ALIF_VBAT, CONFIG_SENSOR_LOG_LEVEL);

/* ADC channel definitions */
#define ADC_CHANNEL_0 (0x00)
#define ADC_CHANNEL_1 (0x01)
#define ADC_CHANNEL_2 (0x02)
#define ADC_CHANNEL_3 (0x03)
#define ADC_CHANNEL_4 (0x04)
#define ADC_CHANNEL_5 (0x05)
#define ADC_CHANNEL_6 (0x06)
#define ADC_CHANNEL_7 (0x07)
#define ADC_CHANNEL_8 (0x08)

/* ADC channel unmask bits */
#define ADC_UNMASK_CHANNEL_0 (1 << 0)
#define ADC_UNMASK_CHANNEL_1 (1 << 1)
#define ADC_UNMASK_CHANNEL_2 (1 << 2)
#define ADC_UNMASK_CHANNEL_3 (1 << 3)
#define ADC_UNMASK_CHANNEL_4 (1 << 4)
#define ADC_UNMASK_CHANNEL_5 (1 << 5)
#define ADC_UNMASK_CHANNEL_6 (1 << 6)
#define ADC_UNMASK_CHANNEL_7 (1 << 7)
#define ADC_UNMASK_CHANNEL_8 (1 << 8)

/* ADC comparator threshold flags */
#define ADC_COMPARATOR_THRESHOLD_ABOVE_A     (1 << 0)
#define ADC_COMPARATOR_THRESHOLD_ABOVE_B     (1 << 1)
#define ADC_COMPARATOR_THRESHOLD_BELOW_A     (1 << 2)
#define ADC_COMPARATOR_THRESHOLD_BELOW_B     (1 << 3)
#define ADC_COMPARATOR_THRESHOLD_BETWEEN_A_B (1 << 4)
#define ADC_COMPARATOR_THRESHOLD_OUTSIDE_A_B (1 << 5)

#define MAX_NUM_THRESHOLD (6)

/* Conversions averaged per fetch */
#define VBAT_ADC_SAMPLES (4)

static uint32_t comp_value[MAX_NUM_THRESHOLD] = {0};
static uint32_t buffer[8];
static uint32_t m_samplings_done;
static uint32_t sample_accum;
static uint8_t comparator;

/* Structure to hold discharge curve points */
struct discharge_point {
	uint16_t voltage_mv;
	uint8_t soc;
};

/* Configuration structure for the VBAT driver */
struct alif_vbat_config {
	const struct device *adc;
	uint32_t channel;
	uint32_t output_ohms;
	uint32_t full_ohms;
	struct gpio_dt_spec enable_gpio;
	struct gpio_dt_spec state_gpio;
	struct gpio_dt_spec charge_control_gpio;
	const struct device *vdd_reg;
	uint32_t power_up_time;
};

/* Data structure for the VBAT driver */
struct alif_vbat_data {
	bool power_enabled;
	int32_t raw_voltage;
	int32_t actual_voltage;
	uint8_t current_soc;
	bool is_charging;
#ifdef CONFIG_ALIF_VBAT_TRIGGER
	const struct device *dev;
	struct gpio_callback gpio_cb;
	const struct sensor_trigger *charge_state_trigger;
	sensor_trigger_handler_t charge_state_handler;
#if defined(CONFIG_ALIF_VBAT_TRIGGER_OWN_THREAD)
	K_KERNEL_STACK_MEMBER(thread_stack, CONFIG_ALIF_VBAT_TRIGGER_THREAD_STACK_SIZE);
	struct k_thread thread;
	struct k_sem gpio_sem;
#elif defined(CONFIG_ALIF_VBAT_TRIGGER_GLOBAL_THREAD)
	struct k_work work;
#endif
#endif
};

#ifdef CONFIG_ALIF_VBAT_TRIGGER
/* Forward declaration of interrupt setup function */
static inline void setup_int(const struct device *dev, bool enable);

#endif

/* ADC callback function for handling comparator thresholds */
static enum adc_action adc_call_back(const struct device *dev, const struct adc_sequence *sequence,
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

	/* Each repeat overwrites the same buffer slot, so accumulate here and
	 * average in vbat_sample_fetch(). */
	sample_accum += buffer[find_lsb_set(sequence->channels) - 1];
	++m_samplings_done;

	if (m_samplings_done < VBAT_ADC_SAMPLES) {
		return ADC_ACTION_REPEAT;
	} else {
		return ADC_ACTION_FINISH;
	}
}

/* Enable or disable power to the VBAT sensor */
static int power_control(const struct device *dev, bool enable)
{
	const struct alif_vbat_config *cfg = dev->config;
	struct alif_vbat_data *data = dev->data;
	int ret = 0;

	if (enable == data->power_enabled) {
		return 0;
	}

	if (enable) {
		if (cfg->vdd_reg) {
			ret = regulator_enable(cfg->vdd_reg);
		} else if (gpio_is_ready_dt(&cfg->enable_gpio)) {
			ret = gpio_pin_set_dt(&cfg->enable_gpio, 1);
		}

		if (ret == 0) {
			k_msleep(cfg->power_up_time);
			data->power_enabled = true;
			LOG_DBG("Power enabled");
		}
	} else {
		if (cfg->vdd_reg) {
			ret = regulator_disable(cfg->vdd_reg);
		} else if (gpio_is_ready_dt(&cfg->enable_gpio)) {
			ret = gpio_pin_set_dt(&cfg->enable_gpio, 0);
		}

		if (ret == 0) {
			data->power_enabled = false;
			LOG_DBG("Power disabled");
		}
	}
	k_msleep(10);

	return ret;
}

/* Control charging enable/disable */
static int charge_control(const struct device *dev, bool enable)
{
	const struct alif_vbat_config *cfg = dev->config;
	int ret = 0;

	if (!gpio_is_ready_dt(&cfg->charge_control_gpio)) {
		LOG_ERR("Charge control GPIO not available");
		return -ENOTSUP;
	}

	if (enable) {
		/* Enable charging: configure GPIO as input */
		ret = gpio_pin_configure_dt(&cfg->charge_control_gpio, GPIO_INPUT);
		if (ret < 0) {
			LOG_ERR("Failed to configure charge control GPIO as input: %d", ret);
			return ret;
		}
		LOG_INF("Charging enabled (GPIO configured as input)");

#ifdef CONFIG_ALIF_VBAT_TRIGGER
		/* Update charging state and trigger callback if registered */
		struct alif_vbat_data *data = dev->data;
		bool was_charging = data->is_charging;
		data->is_charging = !gpio_pin_get_dt(&cfg->state_gpio); /* Read actual charging state */

		/* Trigger callback if charging state changed */
		if (!was_charging && data->is_charging && data->charge_state_handler && data->charge_state_trigger) {
			data->charge_state_handler(dev, data->charge_state_trigger);
		}

		/* Re-enable interrupt when charging is enabled */
		setup_int(dev, true);
#endif
	} else {
		/* Disable charging: configure GPIO as output low */
		ret = gpio_pin_configure_dt(&cfg->charge_control_gpio, GPIO_OUTPUT_LOW);
		if (ret < 0) {
			LOG_ERR("Failed to configure charge control GPIO as output low: %d", ret);
			return ret;
		}
		LOG_INF("Charging disabled (GPIO configured as output low)");

#ifdef CONFIG_ALIF_VBAT_TRIGGER
		/* Disable interrupt when charging is disabled */
		setup_int(dev, false);

		/* Update charging state and trigger callback if registered */
		struct alif_vbat_data *data = dev->data;
		bool was_charging = data->is_charging;
		data->is_charging = false;

		/* Trigger callback if charging state changed from true to false */
		if (was_charging && data->charge_state_handler && data->charge_state_trigger) {
			data->charge_state_handler(dev, data->charge_state_trigger);
		}
#endif
	}

	return 0;
}

/* Set sensor attributes */
static int vbat_attr_set(const struct device *dev, enum sensor_channel chan,
			 enum sensor_attribute attr, const struct sensor_value *val)
{
	switch (attr) {
	case SENSOR_ATTR_CONFIGURATION:
		/* Use configuration to control charging:
		 * val->val1 = 0: disable charging (set GPIO as output low)
		 * val->val1 = 1: enable charging (set GPIO as input) */
		return charge_control(dev, val->val1 ? true : false);
	default:
		return -ENOTSUP;
	}
	return 0;
}

/* Discharge curve: battery mV -> SoC %, derived from a constant-current discharge
 * of a production unit under ~30mA load; see applications/halo/tests/battery.
 * Ascending in both voltage and SoC; SoC is linearly interpolated between points.
 * Replaces the old linear (mV-3200)/9 map, which tracked a LiPo's non-linear
 * open-circuit curve poorly (up to ~20% error vs coulomb-counted charge). */
static const struct discharge_point discharge_curve[] = {
	{ 2920,   0 }, { 3339,   5 }, { 3415,  10 }, { 3484,  15 },
	{ 3531,  20 }, { 3563,  25 }, { 3604,  30 }, { 3630,  35 },
	{ 3664,  40 }, { 3705,  45 }, { 3753,  50 }, { 3796,  55 },
	{ 3849,  60 }, { 3892,  65 }, { 3939,  70 }, { 3995,  75 },
	{ 4035,  80 }, { 4072,  85 }, { 4095,  90 }, { 4112,  95 },
	{ 4128, 100 },  /* top re-anchored to the resting-full voltage (~4130-4142mV)
			 * so a full cell reads 100%; the loaded-discharge start (4200mV)
			 * carries surface charge that isn't real capacity. */
};

/* While charging, the terminal voltage sits above the resting (open-circuit)
 * level by this many mV, as a function of SoC (measured by relax-and-measure on a
 * real unit). We subtract it before the curve lookup so the reported % tracks the
 * resting value and does not jump down when the charger is unplugged. */
struct soc_offset {
	uint8_t soc;
	uint16_t offset_mv;
};
static const struct soc_offset charge_offset[] = {
	{   0, 180 }, {   5, 180 }, {  10, 180 }, {  15, 180 },
	{  20, 169 }, {  25, 150 }, {  30, 134 }, {  35, 126 },
	{  40, 115 }, {  45, 112 }, {  50, 100 }, {  55,  96 },
	{  60,  94 }, {  65, 103 }, {  70, 103 }, {  75,  99 },
	{  80, 104 }, {  85, 113 }, {  90, 115 }, {  95, 118 },
	{ 100,  91 },
};

/* Convert voltage to state of charge (SoC) % by linear interpolation over
 * discharge_curve[]. */
static uint8_t voltage_to_soc(int32_t voltage_mv)
{
	const int n = ARRAY_SIZE(discharge_curve);

	if (voltage_mv <= discharge_curve[0].voltage_mv) {
		return 0;
	}
	if (voltage_mv >= discharge_curve[n - 1].voltage_mv) {
		return 100;
	}

	for (int i = 1; i < n; i++) {
		if (voltage_mv < discharge_curve[i].voltage_mv) {
			const struct discharge_point *lo = &discharge_curve[i - 1];
			const struct discharge_point *hi = &discharge_curve[i];
			int32_t span = hi->voltage_mv - lo->voltage_mv;

			return (uint8_t)(lo->soc +
				((int32_t)(hi->soc - lo->soc) * (voltage_mv - lo->voltage_mv)
				 + span / 2) / span);
		}
	}

	return 100;
}

/* Charging-voltage offset (mV) at a given SoC, interpolated over charge_offset[]. */
static int32_t charge_offset_mv(uint8_t soc)
{
	const int n = ARRAY_SIZE(charge_offset);

	if (soc <= charge_offset[0].soc) {
		return charge_offset[0].offset_mv;
	}
	if (soc >= charge_offset[n - 1].soc) {
		return charge_offset[n - 1].offset_mv;
	}

	for (int i = 1; i < n; i++) {
		if (soc < charge_offset[i].soc) {
			const struct soc_offset *lo = &charge_offset[i - 1];
			const struct soc_offset *hi = &charge_offset[i];
			int32_t span = hi->soc - lo->soc;

			return lo->offset_mv +
				((int32_t)(hi->offset_mv - lo->offset_mv) * (soc - lo->soc)
				 + span / 2) / span;
		}
	}

	return charge_offset[n - 1].offset_mv;
}

/* Fetch sensor data (voltage, SoC, and charging state) */
static int vbat_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	const struct alif_vbat_config *cfg = dev->config;
	struct alif_vbat_data *data = dev->data;
	int ret;

	ret = power_control(dev, true);
	if (ret != 0) {
		LOG_ERR("Failed to enable power: %d", ret);
		return ret;
	}

	struct adc_sequence_options seq_options = {
		.callback = adc_call_back,
		.user_data = (uint8_t *)&comparator,
	};

	struct adc_sequence seq = {.options = &seq_options,
				   .buffer = &buffer,
				   .buffer_size = sizeof(buffer),
				   .channels = (1 << cfg->channel)};

	/* Per-fetch scratch: the callback accumulates VBAT_ADC_SAMPLES
	 * conversions into sample_accum. */
	m_samplings_done = 0;
	sample_accum = 0;
	memset(comp_value, 0, sizeof(comp_value));

	ret = adc_read(cfg->adc, &seq);
	if (ret != 0) {
		LOG_ERR("ADC read failed: %d", ret);
		goto error;
	}

	/* Average the burst, then convert raw ADC counts to voltage */
	int32_t adc_mv = (int32_t)((uint64_t)sample_accum * 1800 /
				   (4095u * VBAT_ADC_SAMPLES));

	data->raw_voltage = adc_mv;
	data->actual_voltage = data->raw_voltage * cfg->full_ohms / cfg->output_ohms;

	int32_t soc_voltage = data->actual_voltage;

	if (data->is_charging) {
		/* Take a first-pass SoC from the elevated charging voltage, subtract the
		 * charging offset at that SoC, then re-evaluate so the reported SoC matches
		 * the resting value the user sees on unplug. */
		uint8_t approx = voltage_to_soc(soc_voltage);

		soc_voltage -= charge_offset_mv(approx);
	}

	data->current_soc = voltage_to_soc(soc_voltage);

error:
	power_control(dev, false);
	return ret;
}

/* Retrieve sensor data for the specified channel */
static int vbat_channel_get(const struct device *dev, enum sensor_channel chan,
			    struct sensor_value *val)
{
	struct alif_vbat_data *data = dev->data;

	switch (chan) {
	case SENSOR_CHAN_VOLTAGE:
		val->val1 = data->raw_voltage;
		val->val2 = 0;
		break;
	case SENSOR_CHAN_GAUGE_VOLTAGE:
		val->val1 = data->actual_voltage;
		val->val2 = 0;
		break;
	case SENSOR_CHAN_GAUGE_STATE_OF_CHARGE:
		val->val1 = data->current_soc;
		val->val2 = 0;
		break;
	case SENSOR_CHAN_GAUGE_STDBY_CURRENT:
		val->val1 = data->is_charging ? 1 : 0; /* 1 for charging, 0 for discharging */
		val->val2 = 0;
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

#ifdef CONFIG_ALIF_VBAT_TRIGGER

/* Enable or disable GPIO interrupt for charging state */
static inline void setup_int(const struct device *dev, bool enable)
{
	const struct alif_vbat_config *cfg = dev->config;
	if (cfg->state_gpio.port && gpio_is_ready_dt(&cfg->state_gpio)) {
		gpio_pin_interrupt_configure_dt(&cfg->state_gpio,
						(enable ? GPIO_INT_EDGE_BOTH : GPIO_INT_DISABLE));
	}
}

/* GPIO callback function for charging state changes */
static void vbat_gpio_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	struct alif_vbat_data *data = CONTAINER_OF(cb, struct alif_vbat_data, gpio_cb);

	ARG_UNUSED(pins);

	setup_int(data->dev, false);

#if defined(CONFIG_ALIF_VBAT_TRIGGER_OWN_THREAD)
	k_sem_give(&data->gpio_sem);
#elif defined(CONFIG_ALIF_VBAT_TRIGGER_GLOBAL_THREAD)
	k_work_submit(&data->work);
#endif
}

/* Thread callback to handle charging state interrupt */
static void vbat_thread_cb(const struct device *dev)
{
	struct alif_vbat_data *data = dev->data;
	const struct alif_vbat_config *cfg = dev->config;
	int err;

	data->is_charging = !gpio_pin_get_dt(&cfg->state_gpio);

	/* Fetch the latest sensor data */
	err = vbat_sample_fetch(dev, SENSOR_CHAN_ALL);
	if (err < 0) {
		LOG_ERR("Sample fetch failed: %d", err);
		return;
	}

	/* Trigger the handler if set */
	if (data->charge_state_handler && data->charge_state_trigger) {

		data->charge_state_handler(dev, data->charge_state_trigger);
	}

	/* Re-enable interrupt */
	setup_int(dev, true);
}

#ifdef CONFIG_ALIF_VBAT_TRIGGER_OWN_THREAD
/* Thread function for handling interrupts in a dedicated thread */
static void vbat_thread(void *p1, void *p2, void *p3)
{
	struct alif_vbat_data *data = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		k_sem_take(&data->gpio_sem, K_FOREVER);
		vbat_thread_cb(data->dev);
	}
}
#endif

#ifdef CONFIG_ALIF_VBAT_TRIGGER_GLOBAL_THREAD
/* Work callback for handling interrupts in the global workqueue */
static void vbat_work_cb(struct k_work *work)
{
	struct alif_vbat_data *data = CONTAINER_OF(work, struct alif_vbat_data, work);

	vbat_thread_cb(data->dev);
}
#endif

/* Configure the charging state trigger */
int vbat_trigger_set(const struct device *dev, const struct sensor_trigger *trig,
		     sensor_trigger_handler_t handler)
{
	struct alif_vbat_data *data = dev->data;
	const struct alif_vbat_config *cfg = dev->config;

	/* Check if state_gpio is available */
	if (!cfg->state_gpio.port || !gpio_is_ready_dt(&cfg->state_gpio)) {
		LOG_ERR("State GPIO not available or not ready");
		return -ENOTSUP;
	}

	/* Validate handler and trigger type */
	if (handler == NULL || trig->type != SENSOR_TRIG_DELTA) {
		LOG_ERR("Invalid handler or trigger type");
		return -EINVAL;
	}

	/* Disable interrupt before configuring */
	setup_int(dev, false);

	/* Store trigger and handler */
	data->charge_state_trigger = trig;
	data->charge_state_handler = handler;

	/* Re-enable interrupt */
	setup_int(dev, true);

	return 0;
}

/* Initialize interrupt handling for charging state */
int vbat_init_interrupt(const struct device *dev)
{
	struct alif_vbat_data *data = dev->data;
	const struct alif_vbat_config *cfg = dev->config;
	int err;

	/* Configure state_gpio for interrupt if available */
	if (cfg->state_gpio.port && gpio_is_ready_dt(&cfg->state_gpio)) {
		err = gpio_pin_configure_dt(&cfg->state_gpio, GPIO_INPUT);
		if (err < 0) {
			LOG_ERR("Failed to configure state GPIO: %d", err);
			return err;
		}

		/* Initialize GPIO callback */
		gpio_init_callback(&data->gpio_cb, vbat_gpio_callback, BIT(cfg->state_gpio.pin));
		err = gpio_add_callback(cfg->state_gpio.port, &data->gpio_cb);
		if (err < 0) {
			LOG_ERR("Failed to set GPIO callback: %d", err);
			return err;
		}
	} else {
		LOG_ERR("State GPIO not available or not ready");
		return -ENODEV;
	}

	/* Store device pointer */
	data->dev = dev;

#if defined(CONFIG_ALIF_VBAT_TRIGGER_OWN_THREAD)
	/* Initialize semaphore and thread for dedicated interrupt handling */
	k_sem_init(&data->gpio_sem, 0, K_SEM_MAX_LIMIT);
	k_thread_create(&data->thread, data->thread_stack,
			CONFIG_ALIF_VBAT_TRIGGER_THREAD_STACK_SIZE, vbat_thread, data, NULL, NULL,
			K_PRIO_COOP(CONFIG_ALIF_VBAT_TRIGGER_THREAD_PRIORITY), 0, K_NO_WAIT);
#elif defined(CONFIG_ALIF_VBAT_TRIGGER_GLOBAL_THREAD)
	/* Initialize work item for global workqueue */
	k_work_init(&data->work, vbat_work_cb);
#endif

	/* Enable interrupt */
	setup_int(dev, true);

	return 0;
}

#endif /* CONFIG_ALIF_VBAT_TRIGGER */

/* Initialize the VBAT sensor */
static int vbat_init(const struct device *dev)
{
	const struct alif_vbat_config *cfg = dev->config;
	struct alif_vbat_data *data = dev->data;
	int ret;

	const struct adc_channel_cfg channel_cfg = {
		.differential = 0,
		.channel_id = cfg->channel,
	};

	/* Configure enable GPIO if available */
	if (gpio_is_ready_dt(&cfg->enable_gpio)) {
		ret = gpio_pin_configure_dt(&cfg->enable_gpio, GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			LOG_ERR("Power GPIO config failed: %d", ret);
			return ret;
		}
	}

	/* Configure charge control GPIO if available */
	if (gpio_is_ready_dt(&cfg->charge_control_gpio)) {
		/* Default to charging enabled: configure as input */
		ret = gpio_pin_configure_dt(&cfg->charge_control_gpio, GPIO_INPUT);
		if (ret < 0) {
			LOG_ERR("Charge control GPIO config failed: %d", ret);
			return ret;
		}
	}

	/* Check if regulator is ready */
	if (cfg->vdd_reg && !device_is_ready(cfg->vdd_reg)) {
		LOG_ERR("Regulator not ready");
		return -ENODEV;
	}

	/* Check if ADC is ready */
	if (!device_is_ready(cfg->adc)) {
		LOG_ERR("ADC not ready");
		return -ENODEV;
	}

	/* Setup ADC channel */
	ret = adc_channel_setup(cfg->adc, &channel_cfg);
	if (ret < 0) {
		LOG_ERR("ADC channel setup failed: %d", ret);
		return ret;
	}

	/* Configure state GPIO for input */
	if (gpio_is_ready_dt(&cfg->state_gpio)) {
		ret = gpio_pin_configure_dt(&cfg->state_gpio, GPIO_INPUT);
		if (ret < 0) {
			LOG_ERR("State GPIO config failed: %d", ret);
			return ret;
		}
	}

	data->is_charging = !gpio_pin_get_dt(&cfg->state_gpio);

#ifdef CONFIG_ALIF_VBAT_TRIGGER
	/* Initialize interrupt handling if enabled */
	if (cfg->state_gpio.port) {
		ret = vbat_init_interrupt(dev);
		if (ret < 0) {
			LOG_ERR("Failed to initialize interrupts: %d", ret);
			return ret;
		}
	}
#endif

	LOG_INF("ALIF VBAT initialized successfully");
	return 0;
}

#ifdef CONFIG_PM_DEVICE
/* Handle power management actions */
static int vbat_pm_action(const struct device *dev, enum pm_device_action action)
{
	int ret;

	pm_device_state_lock(dev);
	switch (action) {
	case PM_DEVICE_ACTION_TURN_OFF:
		ret = power_control(dev, false);
		if (ret < 0) {
			LOG_ERR("Failed to turn off device: %d", ret);
		}
		break;
	case PM_DEVICE_ACTION_TURN_ON:
		ret = power_control(dev, true);
		if (ret < 0) {
			LOG_ERR("Failed to turn on device: %d", ret);
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

/* Sensor driver API definition */
static const struct sensor_driver_api vbat_api = {
	.attr_set = vbat_attr_set,
	.sample_fetch = vbat_sample_fetch,
	.channel_get = vbat_channel_get,
#ifdef CONFIG_ALIF_VBAT_TRIGGER
	.trigger_set = vbat_trigger_set,
#endif
};

/* Macro to define VBAT sensor instance */
#define ALIF_VBAT_INIT(inst)                                                                       \
	static struct alif_vbat_data vbat_data_##inst;                                             \
	static const struct alif_vbat_config vbat_config_##inst = {                                \
		.adc = DEVICE_DT_GET_OR_NULL(DT_INST_PHANDLE(inst, adc)),                          \
		.channel = DT_INST_PROP(inst, channel),                                            \
		.output_ohms = DT_INST_PROP(inst, output_ohms),                                    \
		.full_ohms = DT_INST_PROP(inst, full_ohms),                                        \
		.enable_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, enable_gpios, {0}),                  \
		.state_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, state_gpios, {0}),                    \
		.charge_control_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, charge_control_gpios, {0}),  \
		.vdd_reg = DEVICE_DT_GET_OR_NULL(DT_INST_PHANDLE(inst, vin_supply)),               \
		.power_up_time = DT_INST_PROP(inst, power_up_time_ms),                             \
	};                                                                                         \
	PM_DEVICE_DT_INST_DEFINE(inst, vbat_pm_action);                                            \
	SENSOR_DEVICE_DT_INST_DEFINE(inst, vbat_init, PM_DEVICE_DT_INST_GET(inst),                 \
				     &vbat_data_##inst, &vbat_config_##inst, POST_KERNEL,          \
				     CONFIG_SENSOR_INIT_PRIORITY, &vbat_api);

DT_INST_FOREACH_STATUS_OKAY(ALIF_VBAT_INIT)