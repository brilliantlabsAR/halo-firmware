/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>

#include "lua.h"
#include "lauxlib.h"

#include <drivers/bma580_sensor.h>

#include <halo/lua_imu.h>
#include <halo/lua_service.h>
#include <halo/lua_runtime.h>
#include <halo/pm_manager.h>

LOG_MODULE_REGISTER(lua_imu, CONFIG_HALO_LOG_LEVEL);

/* Device tree nodes */
#define ACCEL_NODE              DT_CHOSEN(zephyr_accel)
#define MAGN_NODE               DT_CHOSEN(zephyr_magn)

#define PI 3.1415926

/**
 * @brief IMU device handles and state
 */
static struct {
	const struct device *magn_dev;
	const struct device *accel_dev;
	int tap_callback_ref;              /* LUA_REGISTRYINDEX reference for tap callback */
	bool devices_ready;                /* Devices are ready but not configured */
	bool hardware_configured;          /* Hardware has been fully configured */
	int trigger_err;                   /* Last sensor_trigger_set() result (0 = tap trigger armed) */
} imu_state = {
	.magn_dev = NULL,
	.accel_dev = NULL,
	.tap_callback_ref = LUA_NOREF,
	.devices_ready = false,
	.hardware_configured = false,
	.trigger_err = 0,
};

/* Forward declaration - defined by HALO_LUA_SERVICE_DEFINE below */
static struct halo_lua_service imu_service;

/* The three hardware tap gestures. The driver stores these trigger pointers,
 * so they must outlive the sensor_trigger_set() call (static, not stack). */
static const struct sensor_trigger tap_triggers[] = {
	{ .type = SENSOR_TRIG_TAP, .chan = SENSOR_CHAN_ACCEL_XYZ },
	{ .type = SENSOR_TRIG_DOUBLE_TAP, .chan = SENSOR_CHAN_ACCEL_XYZ },
	{ .type = (enum sensor_trigger_type)BMA580_TRIG_TRIPLE_TAP,
	  .chan = SENSOR_CHAN_ACCEL_XYZ },
};

static const char *const tap_kind_names[] = { "single", "double", "triple" };

/* Pending tap events, produced by the sensor work thread and drained on the
 * Lua thread by the hook handler. Entries are indexes into tap_kind_names.
 * Single producer / single consumer; monotonically increasing indices. */
#define TAP_RING_SIZE 8 /* power of two */
static struct {
	uint8_t kind[TAP_RING_SIZE];
	atomic_t head; /* next write slot */
	atomic_t tail; /* next read slot */
} tap_ring;

/**
 * @brief Tap event callback handler (hook-based async callback)
 *
 * Called when a tap/motion event is detected by the accelerometer. Drains
 * all pending tap events, invoking the Lua callback once per event with the
 * gesture kind ('single' | 'double' | 'triple') as its argument.
 */
static void lua_imu_tap_callback_handler(lua_State *L, lua_Debug *ar)
{
	ARG_UNUSED(ar);

	/* Clear the hook immediately */
	lua_sethook(L, NULL, 0, 0);

	while (atomic_get(&tap_ring.tail) != atomic_get(&tap_ring.head)) {
		uint8_t kind = tap_ring.kind[atomic_get(&tap_ring.tail) & (TAP_RING_SIZE - 1)];
		atomic_inc(&tap_ring.tail);

		if (imu_state.tap_callback_ref == LUA_NOREF ||
		    kind >= ARRAY_SIZE(tap_kind_names)) {
			continue;
		}

		/* Call the callback with the gesture kind */
		lua_rawgeti(L, LUA_REGISTRYINDEX, imu_state.tap_callback_ref);
		lua_pushstring(L, tap_kind_names[kind]);
		if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
			const char *error = lua_tostring(L, -1);
			LOG_ERR("IMU tap callback error: %s", error);
			lua_pop(L, 1);
		}
	}
}

/**
 * @brief Disarm the hardware tap/motion trigger on the accelerometer.
 *
 * Called before deep sleep and on Lua INTERRUPT/DEINIT to prevent the
 * accelerometer interrupt line (LPGPIO0) from keeping EWIC asserted and
 * causing a spurious warm boot after deep sleep.
 */
static void imu_disarm_hw_trigger(void)
{
	if (!imu_state.devices_ready || !imu_state.accel_dev) {
		return;
	}

	if (imu_state.hardware_configured) {
		/* Disarm all tap gestures via sensor API (NULL handler = disable). */
		for (size_t i = 0; i < ARRAY_SIZE(tap_triggers); i++) {
			int ret = sensor_trigger_set(imu_state.accel_dev,
						     &tap_triggers[i], NULL);
			if (ret < 0 && ret != -ENOSYS) {
				LOG_WRN("Failed to disarm IMU tap trigger %d: %d",
					(int)i, ret);
			}
		}
		LOG_DBG("IMU tap triggers disarmed");
	}

	/* Suspend the accelerometer chip via PM: writes CMD_SUSPEND over I2C,
	 * de-asserting the INT1 pin and stopping the 800Hz sampling loop.
	 * This prevents I2C bus activity from racing with the SE power-off
	 * sequence and causing a spurious deep-sleep reboot. */
#ifdef CONFIG_PM_DEVICE
	{
		int rc = pm_device_action_run(imu_state.accel_dev, PM_DEVICE_ACTION_SUSPEND);
		if (rc < 0 && rc != -EALREADY && rc != -ENOSYS) {
			LOG_WRN("Failed to PM-suspend accelerometer: %d", rc);
		}
	}
	/* Suspend magnetometer too (200Hz samples also produce I2C traffic). */
	if (imu_state.magn_dev) {
		int rc = pm_device_action_run(imu_state.magn_dev, PM_DEVICE_ACTION_SUSPEND);
		if (rc < 0 && rc != -EALREADY && rc != -ENOSYS) {
			LOG_WRN("Failed to PM-suspend magnetometer: %d", rc);
		}
	}
#endif

	/* Mark hardware as unconfigured so next Lua session re-inits fully. */
	imu_state.hardware_configured = false;

	/* Mark service as inactive - tap trigger no longer armed. */
	imu_service.is_active = false;
}

/**
 * @brief Zephyr sensor trigger handler for tap/motion events
 * 
 * This is called from interrupt context.
 */
static void imu_trigger_handler(const struct device *dev, const struct sensor_trigger *trig)
{
	ARG_UNUSED(dev);

	/* Wakeup system if sleeping */
	if (halo_pm_is_sleeping()) {
		halo_pm_wakeup(HALO_PM_WAKEUP_IMU);
	}

	lua_State *L = halo_lua_get_state();
	if (!L || !halo_lua_is_running()) {
		return;
	}

	/* Queue the gesture kind for the hook handler (drop when full). */
	uint8_t kind;
	switch ((int)trig->type) {
	case SENSOR_TRIG_TAP:
		kind = 0;
		break;
	case SENSOR_TRIG_DOUBLE_TAP:
		kind = 1;
		break;
	case BMA580_TRIG_TRIPLE_TAP:
		kind = 2;
		break;
	default:
		return;
	}

	if (atomic_get(&tap_ring.head) - atomic_get(&tap_ring.tail) < TAP_RING_SIZE) {
		tap_ring.kind[atomic_get(&tap_ring.head) & (TAP_RING_SIZE - 1)] = kind;
		atomic_inc(&tap_ring.head);
	}

	/* Set hook to trigger callback on next Lua instruction */
	lua_sethook(L, lua_imu_tap_callback_handler,
	           LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE | LUA_MASKCOUNT, 1);
}

/**
 * @brief Arm all three hardware tap triggers (single/double/triple).
 *
 * @return 0 on success, first failing sensor_trigger_set() result otherwise.
 */
static int imu_arm_tap_triggers(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(tap_triggers); i++) {
		int ret = sensor_trigger_set(imu_state.accel_dev, &tap_triggers[i],
					     imu_trigger_handler);
		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

/**
 * @brief Check if IMU devices are ready (called during library init)
 */
static int imu_device_check(void)
{
	if (imu_state.devices_ready) {
		return 0;
	}

	/* Initialize magnetometer */
	imu_state.magn_dev = DEVICE_DT_GET(MAGN_NODE);
	if (!device_is_ready(imu_state.magn_dev)) {
		LOG_ERR("Magnetometer device %s is not ready", imu_state.magn_dev->name);
		return -ENODEV;
	}

	/* Initialize accelerometer */
	imu_state.accel_dev = DEVICE_DT_GET(ACCEL_NODE);
	if (!device_is_ready(imu_state.accel_dev)) {
		LOG_ERR("Accelerometer device %s is not ready", imu_state.accel_dev->name);
		return -ENODEV;
	}

	imu_state.devices_ready = true;

	return 0;
}

/**
 * @brief Initialize IMU hardware configuration (called on first use)
 */
static int imu_hardware_init(void)
{
	if (imu_state.hardware_configured) {
		return 0;
	}

	if (!imu_state.devices_ready) {
		return -ENODEV;
	}

	//if device is suspended, resume it
#ifdef CONFIG_PM_DEVICE
	enum pm_device_state pm_state;
	pm_device_state_get(imu_state.accel_dev, &pm_state);
	if (pm_state == PM_DEVICE_STATE_SUSPENDED) {
		int ret = pm_device_action_run(imu_state.accel_dev, PM_DEVICE_ACTION_RESUME);
		if (ret < 0) {
			LOG_ERR("Failed to resume accelerometer device: %d", ret);
			return ret;
		}
	}
	pm_device_state_get(imu_state.magn_dev, &pm_state);
	if (pm_state == PM_DEVICE_STATE_SUSPENDED) {
		int ret = pm_device_action_run(imu_state.magn_dev, PM_DEVICE_ACTION_RESUME);
		if (ret < 0) {
			LOG_ERR("Failed to resume magnetometer device: %d", ret);
			return ret;
		}
	}
#endif

	/* Configure accelerometer sampling frequency */
	struct sensor_value odr_attr;
	odr_attr.val1 = 800;
	odr_attr.val2 = 0;
	int ret = sensor_attr_set(imu_state.accel_dev, SENSOR_CHAN_ACCEL_XYZ,
	                          SENSOR_ATTR_SAMPLING_FREQUENCY, &odr_attr);
	if (ret < 0) {
		LOG_ERR("Failed to set accelerometer sampling frequency: %d", ret);
		return ret;
	}

	/* Configure tap/motion triggers (single, double, triple) */
	ret = imu_arm_tap_triggers();
	imu_state.trigger_err = ret;
	if (ret != 0) {
		LOG_WRN("Could not set motion trigger: %d", ret);
		/* Non-fatal here: direction()/raw() don't need the trigger.
		 * tap_callback() checks trigger_err and surfaces the failure. */
	}
	
	/* Configure magnetometer sampling frequency */
	odr_attr.val1 = 200;
	odr_attr.val2 = 0;
	ret = sensor_attr_set(imu_state.magn_dev, SENSOR_CHAN_MAGN_XYZ,
	                      SENSOR_ATTR_SAMPLING_FREQUENCY, &odr_attr);
	if (ret < 0) {
		LOG_ERR("Failed to set magnetometer sampling frequency: %d", ret);
		return ret;
	}

	// fetch one sample to ensure devices are operational
	ret = sensor_sample_fetch(imu_state.accel_dev);
	if (ret < 0) {
		LOG_ERR("Failed to fetch initial accelerometer sample: %d", ret);
		return ret;
	}
	ret = sensor_sample_fetch(imu_state.magn_dev);
	if (ret < 0) {
		LOG_ERR("Failed to fetch initial magnetometer sample: %d", ret);
		return ret;
	}

	imu_state.hardware_configured = true;

	return 0;
}

/**
 * @brief Lua: frame.imu.direction()
 * 
 * Get device orientation as pitch, roll, and heading angles.
 * 
 * @param L Lua state
 * @return 1 (table with pitch, roll, heading fields)
 */
static int lua_imu_direction(lua_State *L)
{
	if (!imu_state.devices_ready) {
		return luaL_error(L, "IMU not initialized");
	}

	/* Perform hardware initialization on first use */
	int ret = imu_hardware_init();
	if (ret < 0) {
		return luaL_error(L, "Failed to initialize IMU hardware: %d", ret);
	}

	/* Fetch accelerometer data */
	ret = sensor_sample_fetch(imu_state.accel_dev);
	if (ret < 0) {
		return luaL_error(L, "Failed to fetch accelerometer data: %d", ret);
	}

	/* Get accelerometer values (raw device-frame axes) */
	struct sensor_value accel_x, accel_y, accel_z;
	sensor_channel_get(imu_state.accel_dev, SENSOR_CHAN_ACCEL_X, &accel_x);
	sensor_channel_get(imu_state.accel_dev, SENSOR_CHAN_ACCEL_Y, &accel_y);
	sensor_channel_get(imu_state.accel_dev, SENSOR_CHAN_ACCEL_Z, &accel_z);

	/* Convert to double */
	double dx = sensor_value_to_double(&accel_x);
	double dy = sensor_value_to_double(&accel_y);
	double dz = sensor_value_to_double(&accel_z);

	/* Remap the raw device axes into the host (right, forward, up) frame before
	 * computing tilt. The BMA580 die is mounted so that, in host convention
	 * (+X = right, +Y = forward, +Z = up):
	 *     host_X (right)   = -dev.z
	 *     host_Y (forward) =  dev.y
	 *     host_Z (up)      =  dev.x
	 * This remap is the one verified empirically (Sydney) and used by the SDK's
	 * imu.lua. Computing roll/pitch on the *raw* axes (as this function used to)
	 * treated dev.z as "up", so a worn-level device read roll ~90 instead of 0. */
	double ax = -dz;   /* right   */
	double ay =  dy;   /* forward */
	double az =  dx;   /* up      */

	/* Calculate pitch and roll (in degrees) in the host frame. Level reads 0:
	 * roll  = side tilt about the forward axis (right temple down => +),
	 * pitch = nose tilt about the right axis  (nose down => sign of forward). */
	double roll = atan2(ax, az) * (180.0 / PI);
	double pitch = atan2(ay, az) * (180.0 / PI);

	/* heading is deliberately 0.0, kept in the table for Frame API
	 * compatibility (Frame returned 0 here too). A meaningful compass
	 * heading cannot be computed in firmware alone: it needs per-unit
	 * hard-iron offsets (from a 3-D tumble calibration), the QMC6308->
	 * BMA580 mag/accel alignment matrix, and the local magnetic
	 * declination -- none of which the firmware has. Hosts compute
	 * tilt-compensated heading from frame.imu.raw(); see the SDK's
	 * imu_compass example (Dart) and brilliant_msg heading/calibration
	 * (Python) for the reference implementation. */
	double heading = 0.0;

	/* Create result table */
	lua_newtable(L);

	lua_pushnumber(L, pitch);
	lua_setfield(L, -2, "pitch");

	lua_pushnumber(L, roll);
	lua_setfield(L, -2, "roll");

	lua_pushnumber(L, heading);
	lua_setfield(L, -2, "heading");

	LOG_DBG("IMU direction: pitch %.2f, roll %.2f, heading %.2f", pitch, roll, heading);
	return 1;
}

/**
 * @brief Lua: frame.imu.raw()
 * 
 * Get raw sensor data from accelerometer and magnetometer.
 * Returns data in milli-units (mg for accel, mG for magnetometer).
 * 
 * @param L Lua state
 * @return 1 (table with accelerometer and compass sub-tables)
 */
static int lua_imu_raw(lua_State *L)
{
	if (!imu_state.devices_ready) {
		return luaL_error(L, "IMU not initialized");
	}

	/* Perform hardware initialization on first use */
	int ret = imu_hardware_init();
	if (ret < 0) {
		return luaL_error(L, "Failed to initialize IMU hardware: %d", ret);
	}

	/* Fetch magnetometer data */
	ret = sensor_sample_fetch(imu_state.magn_dev);
	if (ret < 0) {
		return luaL_error(L, "Failed to fetch magnetometer data: %d", ret);
	}

	/* Fetch accelerometer data */
	ret = sensor_sample_fetch(imu_state.accel_dev);
	if (ret < 0) {
		return luaL_error(L, "Failed to fetch accelerometer data: %d", ret);
	}

	/* Get magnetometer values */
	struct sensor_value magn_x, magn_y, magn_z;
	sensor_channel_get(imu_state.magn_dev, SENSOR_CHAN_MAGN_X, &magn_x);
	sensor_channel_get(imu_state.magn_dev, SENSOR_CHAN_MAGN_Y, &magn_y);
	sensor_channel_get(imu_state.magn_dev, SENSOR_CHAN_MAGN_Z, &magn_z);

	/* Get accelerometer values */
	struct sensor_value accel_x, accel_y, accel_z;
	sensor_channel_get(imu_state.accel_dev, SENSOR_CHAN_ACCEL_X, &accel_x);
	sensor_channel_get(imu_state.accel_dev, SENSOR_CHAN_ACCEL_Y, &accel_y);
	sensor_channel_get(imu_state.accel_dev, SENSOR_CHAN_ACCEL_Z, &accel_z);

	/* Create main result table */
	lua_newtable(L);

	/* Create compass sub-table (magnetometer data in mG) */
	lua_newtable(L);
	lua_pushnumber(L, sensor_value_to_double(&magn_x) * 1000.0);
	lua_setfield(L, -2, "x");
	lua_pushnumber(L, sensor_value_to_double(&magn_y) * 1000.0);
	lua_setfield(L, -2, "y");
	lua_pushnumber(L, sensor_value_to_double(&magn_z) * 1000.0);
	lua_setfield(L, -2, "z");
	lua_setfield(L, -2, "compass");

	/* Create accelerometer sub-table (acceleration data in mg) */
	lua_newtable(L);
	lua_pushnumber(L, sensor_value_to_double(&accel_x) * 1000.0);
	lua_setfield(L, -2, "x");
	lua_pushnumber(L, sensor_value_to_double(&accel_y) * 1000.0);
	lua_setfield(L, -2, "y");
	lua_pushnumber(L, sensor_value_to_double(&accel_z) * 1000.0);
	lua_setfield(L, -2, "z");
	lua_setfield(L, -2, "accelerometer");

	return 1;
}

/**
 * @brief Lua: frame.imu.tap_callback(function)
 * 
 * Register or clear tap detection callback.
 * 
 * @param L Lua state
 * @return 0
 */
static int lua_imu_tap_callback(lua_State *L)
{
	/* Check if clearing callback (nil argument) */

	// initialize IMU hardware
	if (imu_state.devices_ready) {
		int ret = imu_hardware_init();
		if (ret < 0) {
			return luaL_error(L, "Failed to initialize IMU hardware: %d", ret);
		}
	} else {
		return luaL_error(L, "IMU not initialized");
	}

	if (lua_isnil(L, 1)) {
		if (imu_state.tap_callback_ref != LUA_NOREF) {
			luaL_unref(L, LUA_REGISTRYINDEX, imu_state.tap_callback_ref);
			imu_state.tap_callback_ref = LUA_NOREF;
		}
		/* Also disarm hardware interrupt so LPGPIO0 stops asserting. */
		imu_disarm_hw_trigger();
		LOG_DBG("IMU tap callback cleared");
		return 0;
	}

	/* Must be a function */
	if (!lua_isfunction(L, 1)) {
		return luaL_error(L, "expected function or nil");
	}

	/* Registering against an unarmed trigger would succeed silently and
	 * the callback would just never fire — surface the arming failure
	 * (init treats it as non-fatal so direction()/raw() keep working).
	 * Retry the arm first: the init-time failure may have been transient,
	 * and nothing else re-attempts it until a sleep/interrupt cycle
	 * clears hardware_configured. */
	if (imu_state.trigger_err != 0) {
		imu_state.trigger_err = imu_arm_tap_triggers();
		if (imu_state.trigger_err != 0) {
			return luaL_error(L, "tap trigger not armed: %d",
					  imu_state.trigger_err);
		}
	}

	/* Unref old callback if exists */
	if (imu_state.tap_callback_ref != LUA_NOREF) {
		luaL_unref(L, LUA_REGISTRYINDEX, imu_state.tap_callback_ref);
	}

	/* Store new callback in registry */
	imu_state.tap_callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	/* Mark service as active so SUSPEND is dispatched when tap is armed. */
	imu_service.is_active = true;

	LOG_DBG("IMU tap callback registered");
	return 0;
}

/**
 * @brief Lua: frame.imu.config(options)
 * 
 * Configure IMU sensor parameters (sampling frequency and full scale range).
 * 
 * Options table format:
 * {
 *   accelerometer = {
 *     sampling_frequency = 200,  -- Hz (optional)
 *     full_scale = 16            -- g (optional)
 *   },
 *   magnetometer = {
 *     sampling_frequency = 100,  -- Hz (optional)
 *     full_scale = 4             -- Gauss (optional)
 *   }
 * }
 * 
 * @param L Lua state
 * @return 0
 */
static int lua_imu_config(lua_State *L)
{
	if (!imu_state.devices_ready) {
		return luaL_error(L, "IMU not initialized");
	}

	/* Perform hardware initialization on first use */
	int ret = imu_hardware_init();
	if (ret < 0) {
		return luaL_error(L, "Failed to initialize IMU hardware: %d", ret);
	}

	/* Argument must be a table */
	if (!lua_istable(L, 1)) {
		return luaL_error(L, "expected table argument");
	}

	/* Configure accelerometer */
	lua_getfield(L, 1, "accelerometer");
	if (lua_istable(L, -1)) {
		/* Get sampling frequency */
		lua_getfield(L, -1, "sampling_frequency");
		if (lua_isnumber(L, -1)) {
			int freq = lua_tointeger(L, -1);
			struct sensor_value odr_attr;
			odr_attr.val1 = freq;
			odr_attr.val2 = 0;

			ret = sensor_attr_set(imu_state.accel_dev, SENSOR_CHAN_ACCEL_XYZ,
			                      SENSOR_ATTR_SAMPLING_FREQUENCY, &odr_attr);
			if (ret < 0) {
				lua_pop(L, 2); /* pop sampling_frequency and accelerometer */
				return luaL_error(L, "failed to set accelerometer sampling frequency: %d", ret);
			}
		}
		lua_pop(L, 1); /* pop sampling_frequency */

		/* Get full scale */
		lua_getfield(L, -1, "full_scale");
		if (lua_isnumber(L, -1)) {
			int scale = lua_tointeger(L, -1);
			struct sensor_value scale_attr;
			scale_attr.val1 = scale;
			scale_attr.val2 = 0;

			ret = sensor_attr_set(imu_state.accel_dev, SENSOR_CHAN_ACCEL_XYZ,
			                      SENSOR_ATTR_FULL_SCALE, &scale_attr);
			if (ret < 0) {
				lua_pop(L, 2); /* pop full_scale and accelerometer */
				return luaL_error(L, "failed to set accelerometer full scale: %d", ret);
			}
		}
		lua_pop(L, 1); /* pop full_scale */
	}
	lua_pop(L, 1); /* pop accelerometer */

	/* Configure magnetometer */
	lua_getfield(L, 1, "magnetometer");
	if (lua_istable(L, -1)) {
		/* Get sampling frequency */
		lua_getfield(L, -1, "sampling_frequency");
		if (lua_isnumber(L, -1)) {
			int freq = lua_tointeger(L, -1);
			struct sensor_value odr_attr;
			odr_attr.val1 = freq;
			odr_attr.val2 = 0;

			ret = sensor_attr_set(imu_state.magn_dev, SENSOR_CHAN_MAGN_XYZ,
			                      SENSOR_ATTR_SAMPLING_FREQUENCY, &odr_attr);
			if (ret < 0) {
				lua_pop(L, 2); /* pop sampling_frequency and magnetometer */
				return luaL_error(L, "failed to set magnetometer sampling frequency: %d", ret);
			}
		}
		lua_pop(L, 1); /* pop sampling_frequency */

		/* Get full scale */
		lua_getfield(L, -1, "full_scale");
		if (lua_isnumber(L, -1)) {
			int scale = lua_tointeger(L, -1);
			struct sensor_value scale_attr;
			scale_attr.val1 = scale;
			scale_attr.val2 = 0;

			ret = sensor_attr_set(imu_state.magn_dev, SENSOR_CHAN_MAGN_XYZ,
			                      SENSOR_ATTR_FULL_SCALE, &scale_attr);
			if (ret < 0) {
				lua_pop(L, 2); /* pop full_scale and magnetometer */
				return luaL_error(L, "failed to set magnetometer full scale: %d", ret);
			}
		}
		lua_pop(L, 1); /* pop full_scale */
	}
	lua_pop(L, 1); /* pop magnetometer */

	return 0;
}

/* Tap-detector tuning parameters exposed to Lua, in raw chip units except
 * mode and axis which take strings. Order is irrelevant; nil-able. */
static const struct {
	const char *key;
	enum bma580_sensor_attribute attr;
} tap_config_keys[] = {
	{ "threshold", BMA580_ATTR_TAP_PEAK_THRES },
	{ "max_peaks", BMA580_ATTR_TAP_MAX_PEAKS },
	{ "gesture_duration", BMA580_ATTR_TAP_MAX_GESTURE_DUR },
	{ "wait_for_timeout", BMA580_ATTR_TAP_WAIT_FOR_TIMEOUT },
	{ "peak_duration", BMA580_ATTR_TAP_MAX_DUR_BETWEEN_PEAKS },
	{ "shock_duration", BMA580_ATTR_TAP_SHOCK_SETTLING_DUR },
	{ "quiet_between_taps", BMA580_ATTR_TAP_MIN_QUIET_DUR },
	{ "quiet_after_gesture", BMA580_ATTR_TAP_QUIET_TIME_AFTER_GESTURE },
};

static const char *const tap_mode_names[] = { "sensitive", "normal", "robust" };
static const char *const tap_axis_names[] = { "x", "y", "z" };

/**
 * @brief Lua: frame.imu.tap_config([options])
 *
 * With a table argument, sets the given tap-detector parameters (others are
 * left unchanged). With no argument, returns the current configuration as a
 * table. String keys: mode ('sensitive'|'normal'|'robust'), axis
 * ('x'|'y'|'z'). Numeric keys (raw chip units): threshold, max_peaks,
 * gesture_duration, peak_duration, shock_duration, quiet_between_taps,
 * quiet_after_gesture. Boolean key: wait_for_timeout.
 *
 * @param L Lua state
 * @return 0 (setter) or 1 (getter, table)
 */
static int lua_imu_tap_config(lua_State *L)
{
	if (!imu_state.devices_ready) {
		return luaL_error(L, "IMU not initialized");
	}

	/* Perform hardware initialization on first use */
	int ret = imu_hardware_init();
	if (ret < 0) {
		return luaL_error(L, "Failed to initialize IMU hardware: %d", ret);
	}

	struct sensor_value v = { 0 };

	if (lua_isnoneornil(L, 1)) {
		/* Getter: build a table from the current driver state. */
		lua_newtable(L);

		for (size_t i = 0; i < ARRAY_SIZE(tap_config_keys); i++) {
			ret = sensor_attr_get(imu_state.accel_dev, SENSOR_CHAN_ACCEL_XYZ,
					      (enum sensor_attribute)tap_config_keys[i].attr, &v);
			if (ret < 0) {
				return luaL_error(L, "failed to read %s: %d",
						  tap_config_keys[i].key, ret);
			}
			if (tap_config_keys[i].attr == BMA580_ATTR_TAP_WAIT_FOR_TIMEOUT) {
				lua_pushboolean(L, v.val1);
			} else {
				lua_pushinteger(L, v.val1);
			}
			lua_setfield(L, -2, tap_config_keys[i].key);
		}

		ret = sensor_attr_get(imu_state.accel_dev, SENSOR_CHAN_ACCEL_XYZ,
				      (enum sensor_attribute)BMA580_ATTR_TAP_MODE, &v);
		if (ret == 0 && v.val1 >= 0 && v.val1 < (int)ARRAY_SIZE(tap_mode_names)) {
			lua_pushstring(L, tap_mode_names[v.val1]);
			lua_setfield(L, -2, "mode");
		}

		ret = sensor_attr_get(imu_state.accel_dev, SENSOR_CHAN_ACCEL_XYZ,
				      (enum sensor_attribute)BMA580_ATTR_TAP_AXIS, &v);
		if (ret == 0 && v.val1 >= 0 && v.val1 < (int)ARRAY_SIZE(tap_axis_names)) {
			lua_pushstring(L, tap_axis_names[v.val1]);
			lua_setfield(L, -2, "axis");
		}

		return 1;
	}

	if (!lua_istable(L, 1)) {
		return luaL_error(L, "expected table or nil");
	}

	v.val2 = 0;

	lua_getfield(L, 1, "mode");
	if (!lua_isnil(L, -1)) {
		v.val1 = luaL_checkoption(L, -1, NULL, tap_mode_names);
		ret = sensor_attr_set(imu_state.accel_dev, SENSOR_CHAN_ACCEL_XYZ,
				      (enum sensor_attribute)BMA580_ATTR_TAP_MODE, &v);
		if (ret < 0) {
			return luaL_error(L, "failed to set mode: %d", ret);
		}
	}
	lua_pop(L, 1);

	lua_getfield(L, 1, "axis");
	if (!lua_isnil(L, -1)) {
		v.val1 = luaL_checkoption(L, -1, NULL, tap_axis_names);
		ret = sensor_attr_set(imu_state.accel_dev, SENSOR_CHAN_ACCEL_XYZ,
				      (enum sensor_attribute)BMA580_ATTR_TAP_AXIS, &v);
		if (ret < 0) {
			return luaL_error(L, "failed to set axis: %d", ret);
		}
	}
	lua_pop(L, 1);

	for (size_t i = 0; i < ARRAY_SIZE(tap_config_keys); i++) {
		lua_getfield(L, 1, tap_config_keys[i].key);
		if (lua_isnil(L, -1)) {
			lua_pop(L, 1);
			continue;
		}

		if (tap_config_keys[i].attr == BMA580_ATTR_TAP_WAIT_FOR_TIMEOUT) {
			v.val1 = lua_toboolean(L, -1);
		} else if (lua_isnumber(L, -1)) {
			v.val1 = lua_tointeger(L, -1);
		} else {
			return luaL_error(L, "%s must be a number",
					  tap_config_keys[i].key);
		}

		ret = sensor_attr_set(imu_state.accel_dev, SENSOR_CHAN_ACCEL_XYZ,
				      (enum sensor_attribute)tap_config_keys[i].attr, &v);
		if (ret < 0) {
			return luaL_error(L, "failed to set %s: %d",
					  tap_config_keys[i].key, ret);
		}
		lua_pop(L, 1);
	}

	return 0;
}

/* ============================================================================
 * Power Management Integration
 * ============================================================================ */

/**
 * @brief Service lifecycle event handler
 * 
 * Handles lifecycle events like INIT, DEINIT, INTERRUPT.
 * Note: IMU is an always-on service, no suspend/resume needed.
 */
static int imu_service_event_handler(halo_lua_event_t event, void *user_data)
{
	ARG_UNUSED(user_data);
	
	switch (event) {
	case HALO_LUA_EVENT_INIT:
		imu_state.tap_callback_ref = LUA_NOREF;
		break;
		
	case HALO_LUA_EVENT_DEINIT:
		/* Disarm hardware trigger and clear Lua reference. */
		imu_disarm_hw_trigger();
		imu_state.tap_callback_ref = LUA_NOREF;
		break;

	case HALO_LUA_EVENT_INTERRUPT:
		/* Ctrl+C: disarm hardware trigger so LPGPIO0 stops asserting
		 * before deep sleep is entered. */
		imu_disarm_hw_trigger();
		imu_state.tap_callback_ref = LUA_NOREF;
		break;

	case HALO_LUA_EVENT_SUSPEND:
		halo_pm_sleep_mode_t mode = halo_pm_get_sleep_mode();
		if (mode == HALO_PM_SLEEP_LIGHT || mode == HALO_PM_SLEEP_STANDBY) {
			/* Light sleep / standby: ensure the accelerometer + motion trigger
			 * are armed so any movement can wake the system via
			 * imu_trigger_handler → halo_pm_wakeup(IMU).
			 *
			 * If the user never called frame.imu.*, the hardware has
			 * not been initialised yet — do it now so the GPIO
			 * interrupt is ready before k_sleep(). */
			if (!imu_state.devices_ready) {
				imu_device_check();
			}
			if (imu_state.devices_ready && !imu_state.hardware_configured) {
				int rc = imu_hardware_init();
				if (rc < 0) {
					LOG_WRN("Failed to init IMU for sleep wake: %d", rc);
				}
			}

			/* Suspend the magnetometer to save power. */
#ifdef CONFIG_PM_DEVICE
			if (imu_state.magn_dev) {
				int rc = pm_device_action_run(imu_state.magn_dev,
							     PM_DEVICE_ACTION_SUSPEND);
				if (rc < 0 && rc != -EALREADY && rc != -ENOSYS) {
					LOG_WRN("Failed to PM-suspend magnetometer: %d", rc);
				}
			}
#endif
		} else {
			/* Deep sleep: full disarm to prevent LPGPIO0 keeping
			 * EWIC asserted → spurious warm boot. */
			imu_disarm_hw_trigger();
		}
		break;

	case HALO_LUA_EVENT_RESUME:
		/* Nothing to do - tap callback is re-registered by re-running Lua script. */
		break;

	default:
		break;
	}
	
	return 0;
}

/* Register service with lifecycle management (always-on service) */
HALO_LUA_SERVICE_DEFINE(imu_service, imu_service_event_handler, NULL, true);

/**
 * @brief Open and register IMU library with Lua VM
 * 
 * Registers all IMU functions under frame.imu table and
 * initializes the IMU hardware.
 */
int lua_open_imu_library(lua_State *L)
{

	/* Register service for lifecycle management */
	int ret = halo_lua_service_register(&imu_service);
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("Failed to register IMU service: %d", ret);
		return ret;
	}

	/* Check IMU devices (defer actual hardware configuration to first use) */
	ret = imu_device_check();
	if (ret < 0) {
		LOG_ERR("Failed to check IMU devices: %d", ret);
		return ret;
	}

	/* Get or create the global 'frame' table */
	lua_getglobal(L, "frame");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setglobal(L, "frame");
	}

	/* Create 'imu' subtable */
	lua_newtable(L);

	/* Register IMU functions */
	lua_pushcfunction(L, lua_imu_tap_callback);
	lua_setfield(L, -2, "tap_callback");

	lua_pushcfunction(L, lua_imu_direction);
	lua_setfield(L, -2, "direction");

	lua_pushcfunction(L, lua_imu_raw);
	lua_setfield(L, -2, "raw");

	lua_pushcfunction(L, lua_imu_config);
	lua_setfield(L, -2, "config");

	lua_pushcfunction(L, lua_imu_tap_config);
	lua_setfield(L, -2, "tap_config");

	/* Set 'imu' table in 'frame' */
	lua_setfield(L, -2, "imu");

	/* Pop frame table */
	lua_pop(L, 1);

	return 0;
}
