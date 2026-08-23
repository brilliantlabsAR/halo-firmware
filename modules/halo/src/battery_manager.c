/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/led.h>
#include <zephyr/sys/dlist.h>
#include <string.h>

#include "halo/battery_manager.h"
#include "halo/led_manager.h"
#include "halo/pm_manager.h"

LOG_MODULE_REGISTER(halo_battery, CONFIG_HALO_LOG_LEVEL);

/* EMA Filter configuration */
#define BATTERY_FILTER_ALPHA_U16   6553    /* Alpha = 65535/(N+1), N=9 for ~10 sample window */
#define FILTER_INIT_CYCLES         3       /* Number of cycles to initialize EMA */
#define BATTERY_SEED_DELAY_MS      15000   /* Seed the EMA from post-boot readings only */
#define BATTERY_CONVERGE_CONFIRM   3       /* Counter-direction samples before converging */
#define BATTERY_CONVERGE_STEP      1       /* Max %-points per update against direction */

/* Battery manager context */
static struct {
	bool initialized;
	struct halo_battery_info info;

	/* Devices */
	const struct device *vbat_dev;

	/* Sensor data */
	struct sensor_value voltage;
	struct sensor_value soc;
	struct sensor_value charge_status;

	/* EMA filtering */
	uint8_t battery_percentage_ema;    /* Current EMA value */
	bool ema_initialized;              /* EMA initialization flag */
	uint8_t ema_init_counter;          /* Counter for init cycles */
	uint8_t converge_counter;          /* Consecutive counter-direction samples */

	/* Update management */
	struct k_work_delayable update_work;
	uint32_t update_interval_ms;

	/* Event callbacks */
	sys_dlist_t callbacks;
	struct k_mutex lock;

	/* Thresholds */
	uint8_t low_battery_threshold;
	uint8_t critical_battery_threshold;
#ifdef CONFIG_HALO_PM_MANAGER
	struct halo_pm_callback pm_cb;
#endif
} bat_mgr_ctx = {
	.initialized = false,
	.update_interval_ms = CONFIG_HALO_BATTERY_UPDATE_INTERVAL_MS,
	.low_battery_threshold = 20,
	.critical_battery_threshold = 10,
	.battery_percentage_ema = 0,
	.ema_initialized = false,
	.ema_init_counter = 0,
};

/* Forward declarations */
static void battery_update_work_fn(struct k_work *work);
static void battery_trigger_handler(const struct device *dev, const struct sensor_trigger *trig);

/**
 * @brief Update EMA filter with new battery percentage value
 * 
 * Uses exponential moving average with special handling for edge cases
 * near 0% and 100% to ensure smooth transitions.
 * 
 * @param current_ema Current EMA value
 * @param new_value New raw percentage reading
 * @param is_charging True if currently charging
 * @return Filtered percentage value
 */
static uint8_t update_ema_filter(uint8_t current_ema, uint8_t new_value, bool is_charging)
{
	/* Near the endpoints, step at most 1% per update in the charge direction
	 * so the last few percent move smoothly. Readings against the charge
	 * direction fall through to the plain EMA and are rate-limited by the
	 * convergence path in apply_battery_filter(). */
	if (!is_charging && current_ema <= 5 && new_value < current_ema) {
		return current_ema - 1;
	}
	if (is_charging && current_ema >= 95 && new_value > current_ema) {
		return current_ema + 1;
	}

	/* Constant coefficient Alpha for EMA calculation, scaled to 16 bit.
	 * Alpha = 65535/(N+1) where N is the averaging window */
	const uint32_t alpha = BATTERY_FILTER_ALPHA_U16;
	const uint32_t alpha_complement = UINT16_MAX - BATTERY_FILTER_ALPHA_U16;

	/* Calculate new EMA: new_ema = (alpha * new_value + alpha_complement * current_ema) / 65535 */
	uint64_t new_ema = ((uint64_t)alpha * new_value) + ((uint64_t)alpha_complement * current_ema);

	/* Scale result back to 8-bit, with rounding */
	return (uint8_t)((new_ema + 32768) >> 16);
}

/**
 * @brief Apply battery percentage filter with charge-direction weighting
 *
 * Movement with the charge direction (down while discharging, up while
 * charging) follows the EMA directly; movement against it converges toward
 * the measured value at a bounded rate once confirmed by consecutive
 * samples.
 *
 * @param raw_percentage Raw percentage from sensor
 * @return Filtered percentage value
 */
static uint8_t apply_battery_filter(uint8_t raw_percentage)
{
	uint8_t result;
	bool is_charging = bat_mgr_ctx.info.is_charging;

	/* Initialize EMA with first readings */
	if (!bat_mgr_ctx.ema_initialized) {
		/* Seed only from post-boot steady-state readings; report the raw
		 * value until then. Voltage, charging state and callbacks are
		 * unaffected - only the level filter waits. */
		if (k_uptime_get() < BATTERY_SEED_DELAY_MS) {
			return raw_percentage;
		}

		bat_mgr_ctx.battery_percentage_ema = raw_percentage;
		bat_mgr_ctx.ema_init_counter++;

		/* Run filter for FILTER_INIT_CYCLES to stabilize */
		if (bat_mgr_ctx.ema_init_counter >= FILTER_INIT_CYCLES) {
			bat_mgr_ctx.ema_initialized = true;
			LOG_DBG("EMA filter initialized with value: %d", raw_percentage);
		}

		return raw_percentage;
	}

	/* Apply EMA filter to smooth out percentage changes */
	uint8_t filtered = update_ema_filter(bat_mgr_ctx.battery_percentage_ema,
					     raw_percentage, is_charging);

	/* Movement with the charge direction (down while discharging, up while
	 * charging) is accepted as filtered. Movement against it converges at a
	 * bounded rate: after BATTERY_CONVERGE_CONFIRM consecutive
	 * counter-direction samples, step at most BATTERY_CONVERGE_STEP
	 * %-points per update toward the measured value. */
	uint8_t current = bat_mgr_ctx.battery_percentage_ema;
	bool with_direction = is_charging ? (filtered >= current) : (filtered <= current);

	if (with_direction) {
		result = filtered;
		bat_mgr_ctx.converge_counter = 0;
	} else if (++bat_mgr_ctx.converge_counter >= BATTERY_CONVERGE_CONFIRM) {
		if (filtered > current) {
			result = current + MIN(BATTERY_CONVERGE_STEP,
					       (uint8_t)(filtered - current));
		} else {
			result = current - MIN(BATTERY_CONVERGE_STEP,
					       (uint8_t)(current - filtered));
		}
	} else {
		result = current;
	}

	/* Update EMA state */
	bat_mgr_ctx.battery_percentage_ema = result;

	/* Clamp to valid range */
	if (result > 100) {
		result = 100;
	}

	return result;
}

/* Notify registered callbacks */
static void notify_callbacks(enum halo_battery_event event, uint8_t level)
{
	sys_dnode_t *node;
	struct battery_callback *cb;

	k_mutex_lock(&bat_mgr_ctx.lock, K_FOREVER);

	SYS_DLIST_FOR_EACH_NODE(&bat_mgr_ctx.callbacks, node) {
		cb = CONTAINER_OF(node, struct battery_callback, node);
		if (cb->callback) {
			cb->callback(event, level, cb->user_data);
		}
	}

	k_mutex_unlock(&bat_mgr_ctx.lock);
}

/* Update charging LED */
static void update_charge_led(void)
{
#ifdef CONFIG_HALO_LED_MANAGER
	if (!bat_mgr_ctx.info.is_charging) {
		/* Not charging: LED off */
		halo_led_clear_state(HALO_LED_PRIORITY_MEDIUM);
	} else if (bat_mgr_ctx.info.level >= 97) {
		/* Fully charged (>= 97%): LED solid on */
		halo_led_set_state(HALO_LED_STATE_ON, HALO_LED_PRIORITY_MEDIUM);
		LOG_DBG("LED: fully charged (%d%%), solid ON", bat_mgr_ctx.info.level);
	} else {
		/* Charging: LED breathing effect (smooth fade in/out) */
		halo_led_set_state(HALO_LED_STATE_CHARGING, HALO_LED_PRIORITY_MEDIUM);
		LOG_DBG("LED: charging (%d%%), breathing", bat_mgr_ctx.info.level);
	}
#endif
}

/* Fetch battery data from sensor */
static int fetch_battery_data(void)
{
	int ret;
	uint8_t prev_level = bat_mgr_ctx.info.level;
	bool prev_charging = bat_mgr_ctx.info.is_charging;

	if (!bat_mgr_ctx.vbat_dev || !device_is_ready(bat_mgr_ctx.vbat_dev)) {
		return -ENODEV;
	}

	/* Fetch latest sample */
	ret = sensor_sample_fetch(bat_mgr_ctx.vbat_dev);
	if (ret < 0) {
		LOG_ERR("Failed to fetch battery sample: %d", ret);
		return ret;
	}

	/* Get voltage */
	ret = sensor_channel_get(bat_mgr_ctx.vbat_dev, SENSOR_CHAN_GAUGE_VOLTAGE,
				 &bat_mgr_ctx.voltage);
	if (ret < 0) {
		bat_mgr_ctx.voltage.val1 = 0;
	} else {
		bat_mgr_ctx.info.voltage_mv = bat_mgr_ctx.voltage.val1;
	}

	/* Get charging status first (needed for EMA filter direction) */
	ret = sensor_channel_get(bat_mgr_ctx.vbat_dev, SENSOR_CHAN_GAUGE_STDBY_CURRENT,
				 &bat_mgr_ctx.charge_status);
	if (ret < 0) {
		bat_mgr_ctx.charge_status.val1 = 0;
	}
	bat_mgr_ctx.info.is_charging = (bat_mgr_ctx.charge_status.val1 != 0);

	/* Get state of charge (SoC) */
	ret = sensor_channel_get(bat_mgr_ctx.vbat_dev, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE,
				 &bat_mgr_ctx.soc);
	if (ret < 0) {
		bat_mgr_ctx.soc.val1 = 0;
	} else {
		/* Apply EMA filter with charging direction enforcement */
		uint8_t raw_level = (uint8_t)MIN(100, MAX(0, bat_mgr_ctx.soc.val1));
		bat_mgr_ctx.info.level = apply_battery_filter(raw_level);
	}

	/* Notify callbacks on state changes */
	if (bat_mgr_ctx.info.level != prev_level) {
		notify_callbacks(HALO_BATTERY_EVENT_LEVEL_CHANGED, bat_mgr_ctx.info.level);

		/* Check for low/critical battery */
		if (bat_mgr_ctx.info.level <= bat_mgr_ctx.critical_battery_threshold) {
			notify_callbacks(HALO_BATTERY_EVENT_CRITICAL_BATTERY,
					 bat_mgr_ctx.info.level);
		} else if (bat_mgr_ctx.info.level <= bat_mgr_ctx.low_battery_threshold) {
			notify_callbacks(HALO_BATTERY_EVENT_LOW_BATTERY, bat_mgr_ctx.info.level);
		}
	}

	if (bat_mgr_ctx.info.is_charging != prev_charging) {
		if (bat_mgr_ctx.info.is_charging) {
			notify_callbacks(HALO_BATTERY_EVENT_CHARGING_STARTED,
					 bat_mgr_ctx.info.level);
		} else {
			notify_callbacks(HALO_BATTERY_EVENT_CHARGING_STOPPED,
					 bat_mgr_ctx.info.level);
		}
	}

	return 0;
}

/* Sensor trigger handler for charging state changes */
static void battery_trigger_handler(const struct device *dev, const struct sensor_trigger *trig)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(trig);

	/* Fetch battery data */
	fetch_battery_data();

	/* Update LED */
	update_charge_led();
}

/* Periodic battery update work handler */
static void battery_update_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	/* Fetch latest data */
	fetch_battery_data();
	update_charge_led();

	/* Reschedule if still enabled */
	if (bat_mgr_ctx.update_interval_ms > 0) {
		k_work_schedule(&bat_mgr_ctx.update_work, K_MSEC(bat_mgr_ctx.update_interval_ms));
	}
}

#ifdef CONFIG_HALO_PM_MANAGER
/* Power management callback */
static int battery_pm_callback(halo_pm_event_t event, halo_pm_sleep_mode_t mode, void *user_data)
{
	ARG_UNUSED(mode);
	ARG_UNUSED(user_data);

	if (!bat_mgr_ctx.initialized) {
		return 1; /* Skip - not initialized, no resume needed */
	}

	switch (event) {
	case HALO_PM_EVENT_SUSPEND:
		halo_battery_suspend();
		return 0; /* Suspended - needs resume */
	case HALO_PM_EVENT_RESUME:
		halo_battery_resume();
		return 0;
	default:
		break;
	}

	return 1; /* Skip by default */
}
#endif 
/* Public API implementations */

int halo_battery_init(void)
{
	int ret;

	if (bat_mgr_ctx.initialized) {
		return 0;
	}

	/* Initialize mutex and callback list */
	k_mutex_init(&bat_mgr_ctx.lock);
	sys_dlist_init(&bat_mgr_ctx.callbacks);

	/* Get device references */
	bat_mgr_ctx.vbat_dev = DEVICE_DT_GET(DT_NODELABEL(vbat));
	if (!device_is_ready(bat_mgr_ctx.vbat_dev)) {
		LOG_ERR("VBAT device not ready");
		return -ENODEV;
	}

	/* Initialize work */
	k_work_init_delayable(&bat_mgr_ctx.update_work, battery_update_work_fn);

	bat_mgr_ctx.initialized = true;

	/* Immediate battery read + LED update so charging LED shows at boot */
	fetch_battery_data();
	update_charge_led();

	/* Configure charging state trigger AFTER initial read to avoid race
	 * between trigger ISR and init thread corrupting LED driver state */
	struct sensor_trigger trig = {
		.type = SENSOR_TRIG_DELTA,
		.chan = SENSOR_CHAN_GAUGE_STDBY_CURRENT,
	};

	ret = sensor_trigger_set(bat_mgr_ctx.vbat_dev, &trig, battery_trigger_handler);
	if (ret < 0) {
		LOG_WRN("Failed to set battery trigger: %d", ret);
	}

	/* Start periodic updates */
	k_work_schedule(&bat_mgr_ctx.update_work, K_MSEC(bat_mgr_ctx.update_interval_ms));

#ifdef CONFIG_HALO_PM_MANAGER
	/* Register PM callback for sleep/wake handling */
	ret = halo_pm_register_callback(&bat_mgr_ctx.pm_cb, battery_pm_callback, NULL, "battery",
					0);
	if (ret < 0) {
		LOG_WRN("Failed to register battery PM callback: %d", ret);
	}
#endif

	LOG_DBG("Battery manager initialized");

	return 0;
}

int halo_battery_deinit(void)
{
	if (!bat_mgr_ctx.initialized) {
		return 0;
	}

	/* Cancel periodic work */
	k_work_cancel_delayable(&bat_mgr_ctx.update_work);

	/* Clear charging LED state */
	halo_led_clear_state(HALO_LED_PRIORITY_MEDIUM);

	bat_mgr_ctx.initialized = false;

	return 0;
}

int halo_battery_get_level(void)
{
	if (!bat_mgr_ctx.initialized) {
		return -ENODEV;
	}

	return bat_mgr_ctx.info.level;
}

int halo_battery_get_voltage(void)
{
	if (!bat_mgr_ctx.initialized) {
		return -ENODEV;
	}

	return bat_mgr_ctx.info.voltage_mv;
}

bool halo_battery_is_charging(void)
{
	if (!bat_mgr_ctx.initialized) {
		return false;
	}

	return bat_mgr_ctx.info.is_charging;
}

int halo_battery_get_info(struct halo_battery_info *info)
{
	if (!bat_mgr_ctx.initialized || !info) {
		return -EINVAL;
	}

	k_mutex_lock(&bat_mgr_ctx.lock, K_FOREVER);
	memcpy(info, &bat_mgr_ctx.info, sizeof(struct halo_battery_info));
	k_mutex_unlock(&bat_mgr_ctx.lock);

	return 0;
}

int halo_battery_register_callback(struct battery_callback *cb)
{
	if (!cb || !cb->callback) {
		return -EINVAL;
	}

	k_mutex_lock(&bat_mgr_ctx.lock, K_FOREVER);

	/* Check if already registered */
	sys_dnode_t *node;
	struct battery_callback *item;
	SYS_DLIST_FOR_EACH_NODE(&bat_mgr_ctx.callbacks, node) {
		item = CONTAINER_OF(node, struct battery_callback, node);
		if (item == cb) {
			k_mutex_unlock(&bat_mgr_ctx.lock);
			LOG_WRN("Callback already registered");
			return -EALREADY;
		}
	}

	/* Add to list */
	sys_dlist_append(&bat_mgr_ctx.callbacks, &cb->node);

	k_mutex_unlock(&bat_mgr_ctx.lock);

	return 0;
}

int halo_battery_unregister_callback(struct battery_callback *cb)
{
	if (!cb) {
		return -EINVAL;
	}

	k_mutex_lock(&bat_mgr_ctx.lock, K_FOREVER);

	/* Remove from list */
	if (!sys_dlist_is_empty(&bat_mgr_ctx.callbacks)) {
		sys_dlist_remove(&cb->node);
	}

	k_mutex_unlock(&bat_mgr_ctx.lock);

	return 0;
}

int halo_battery_set_update_interval(uint32_t interval_ms)
{
	k_work_cancel_delayable(&bat_mgr_ctx.update_work);

	bat_mgr_ctx.update_interval_ms = interval_ms;

	if (interval_ms > 0 && bat_mgr_ctx.initialized) {
		k_work_schedule(&bat_mgr_ctx.update_work, K_MSEC(interval_ms));
	}

	return 0;
}

int halo_battery_update_now(void)
{
	if (!bat_mgr_ctx.initialized) {
		return -ENODEV;
	}

	int ret = fetch_battery_data();
	if (ret == 0) {
		update_charge_led();
	}

	return ret;
}

int halo_battery_suspend(void)
{
	if (!bat_mgr_ctx.initialized) {
		return -ENODEV;
	}
	// suspend work if needed
	k_work_cancel_delayable(&bat_mgr_ctx.update_work);
	return 0;
}

int halo_battery_resume(void)
{
	if (!bat_mgr_ctx.initialized) {
		return -ENODEV;
	}
	// resume work if needed
	k_work_schedule(&bat_mgr_ctx.update_work, K_MSEC(bat_mgr_ctx.update_interval_ms));
	return 0;
}
