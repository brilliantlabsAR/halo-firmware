/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sm/sm.h>
#include "lua.h"
#include "lauxlib.h"
#include <halo/lua_system.h>
#include <halo/lua_service.h>
#include <halo/battery_manager.h>
#include <halo/pm_manager.h>
#include <se_service.h>
#ifdef CONFIG_HALO_BLE_MANAGER
#include <halo/ble_connection.h>
#endif

LOG_MODULE_REGISTER(lua_system, CONFIG_HALO_LOG_LEVEL);

/* Static state for stay_awake functionality */
static bool stay_awake = false;

/* Interrupt flag for Ctrl+C during sleep */
static volatile bool sleep_interrupted = false;

/**
 * @brief Lua: frame.sleep(seconds)
 *
 * Sleep for specified number of seconds. If called with no arguments,
 * enters deep sleep mode (shutdown).
 *
 * Sleep is broken into chunks to allow Ctrl+C interruption.
 *
 * @param L Lua state
 * @return 0 (no return values)
 */
static int lua_sleep(lua_State *L)
{
	int nargs = lua_gettop(L);

	if (nargs == 0) {
		/* No arguments - enter deep sleep indefinitely */
		LOG_DBG("Entering deep sleep (shutdown)");
		int ret = halo_pm_sleep_deep(0); /* 0 = indefinite */
		if (ret < 0) {
			return luaL_error(L, "failed to enter deep sleep: %d", ret);
		}
		return 0;
	}

	/* Sleep for specified duration */
	lua_Number seconds = luaL_checknumber(L, 1);
	if (seconds < 0) {
		return luaL_error(L, "sleep duration must be non-negative");
	}

	if (seconds == 0) {
		/* Sleep for 0 seconds - enter deep sleep indefinitely */
		LOG_DBG("Entering deep sleep (shutdown)");
		int ret = halo_pm_sleep_deep(0); /* 0 = indefinite */
		if (ret < 0) {
			return luaL_error(L, "failed to enter deep sleep: %d", ret);
		}
		return 0;
	}

	uint32_t total_ms = (uint32_t)(seconds * 1000);

	/* Reset interrupt flag before starting sleep */
	sleep_interrupted = false;

	/* Break sleep into chunks to allow interruption via Ctrl+C */
	/* Each chunk is 100ms, giving responsive interrupt handling */
	const uint32_t chunk_ms = 100;

	while (total_ms > 0 && !sleep_interrupted) {
		uint32_t sleep_ms = (total_ms > chunk_ms) ? chunk_ms : total_ms;

		k_sleep(K_MSEC(sleep_ms));
		total_ms -= sleep_ms;
	}

	/* If interrupted, throw Lua error to break execution */
	if (sleep_interrupted) {
		sleep_interrupted = false; /* Reset flag */
		return luaL_error(L, "interrupted");
	}

	return 0;
}

static int lua_light_sleep(lua_State *L)
{
	int nargs = lua_gettop(L);
	uint32_t timeout_ms = 0;

	if (nargs > 0) {
		lua_Number seconds = luaL_checknumber(L, 1);
		if (seconds < 0) {
			return luaL_error(L, "timeout must be non-negative");
		}
		timeout_ms = (uint32_t)(seconds * 1000);
	}

	LOG_DBG("Entering light sleep, timeout %u ms", timeout_ms);
	int ret = halo_pm_sleep_light(timeout_ms);
	if (ret < 0) {
		return luaL_error(L, "failed to enter light sleep: %d", ret);
	}

	/* Nothing after this point runs: the PM suspend path armed the break
	 * hook, so the VM restarts and main.lua runs from the top on wake.
	 * Read frame.wakeup_source() there to learn why. */
	return 0;
}

static int lua_standby(lua_State *L)
{
	int nargs = lua_gettop(L);
	uint32_t timeout_ms = 0;

	if (nargs > 0) {
		lua_Number seconds = luaL_checknumber(L, 1);
		if (seconds < 0) {
			return luaL_error(L, "timeout must be non-negative");
		}
		timeout_ms = (uint32_t)(seconds * 1000);
	}

	int ret = halo_pm_sleep_standby(timeout_ms);
	if (ret < 0) {
		return luaL_error(L, "failed to enter standby: %d", ret);
	}

	if(sleep_interrupted) {
		sleep_interrupted = false; /* Reset flag */
		return luaL_error(L, "interrupted");
	}

	/* Standby resumes in place: execution continues with the statement
	 * after frame.standby(); frame.wakeup_source() reports the reason. */
	return 0;
}

/**
 * @brief Lua: frame.get_eui()
 *
 * Get the device EUI-64 address.
 * Returns a hexadecimal string (16 characters) representing the 8-byte EUI-64 address.
 * Format: OUI (2C:F7:F1) + 5-byte extension.
 *
 * @param L Lua state
 * @return 1 (the EUI-64 hex string)
 */
static int lua_get_eui(lua_State *L)
{
	uint8_t sn[8];
	char hex_sn[17];  // 8*2 + 1 for null terminator
	int ret;

	ret = se_system_get_eui_extension(false, &sn[3]);
	if (ret) {
		luaL_error(L, "Failed to get EUI-64: %d", ret);
		return 0;
	}

	// Add OUI prefix: 2C:F7:F1
	sn[0] = 0x2C;
	sn[1] = 0xF7;
	sn[2] = 0xF1;

	snprintf(hex_sn, sizeof(hex_sn), "%02X%02X%02X%02X%02X%02X%02X%02X",
	         sn[0], sn[1], sn[2], sn[3], sn[4], sn[5], sn[6], sn[7]);
	lua_pushstring(L, hex_sn);
	return 1;
}

/**
 * @brief Lua: frame.yield()
 *
 * Yield execution to allow other tasks to run. This is a no-op if
 * called from outside a coroutine.
 *
 * @param L Lua state
 * @return 0 (no return values)
 */
static int lua_kyield(lua_State *L)
{
	k_yield();
	return 0; // This line will never be reached
}

/**
 * @brief Lua: frame.stay_awake([enabled])
 *
 * Get or set the stay_awake flag. When enabled, prevents automatic sleep.
 *
 * @param L Lua state
 * @return 0 or 1 (returns current state if no argument provided)
 */
static int lua_stay_awake(lua_State *L)
{
	int nargs = lua_gettop(L);

	if (nargs > 1) {
		return luaL_error(L, "expected 0 or 1 arguments");
	}

	if (nargs == 1) {
		/* Set stay_awake state */
		luaL_checktype(L, 1, LUA_TBOOLEAN);
		stay_awake = lua_toboolean(L, 1);
		LOG_DBG("Stay awake: %s", stay_awake ? "enabled" : "disabled");
		/* Intentionally just a flag the app reads back: taking the
		 * SOFT_OFF policy lock here would only block the explicit
		 * power-off gesture (light/standby never consult that lock),
		 * and the flag reset on runtime restart would leak the count. */
		return 0;
	}

	/* Return current state */
	lua_pushboolean(L, stay_awake);
	return 1;
}

/**
 * @brief Lua: frame.reboot()
 *
 * Reboot the system.
 *
 * @param L Lua state
 * @return 0 (no return values)
 */
static int lua_reboot(lua_State *L)
{
	LOG_DBG("Rebooting system");
#ifdef CONFIG_HALO_BLE_MANAGER
	halo_ble_conn_prepare_reboot();
#endif
	sys_reboot(SYS_REBOOT_COLD);
	return 0; // This line will never be reached
}

/**
 * @brief Lua: frame.battery_level()
 *
 * Get current battery level percentage.
 *
 * @param L Lua state
 * @return 1 (battery level 0-100)
 */
static int lua_battery_level(lua_State *L)
{
	int level = halo_battery_get_level();

	if (level < 0) {
		LOG_WRN("Failed to get battery level: %d", level);
		lua_pushnumber(L, 0);
	} else {
		lua_pushnumber(L, level);
	}

	return 1;
}

/**
 * @brief Lua: frame.battery_voltage()
 *
 * Get current battery voltage in millivolts.
 *
 * @param L Lua state
 * @return 1 (battery voltage in mV)
 */
static int lua_battery_voltage(lua_State *L)
{
	int voltage = halo_battery_get_voltage();

	if (voltage < 0) {
		LOG_WRN("Failed to get battery voltage: %d", voltage);
		lua_pushnumber(L, 0);
	} else {
		lua_pushnumber(L, voltage);
	}

	return 1;
}

/**
 * @brief Lua: frame.battery_charging()
 *
 * Check if battery is currently charging.
 *
 * @param L Lua state
 * @return 1 (true if charging, false otherwise)
 */
static int lua_battery_charging(lua_State *L)
{
	bool charging = halo_battery_is_charging();
	lua_pushboolean(L, charging);
	return 1;
}

/**
 * @brief Lua: frame.ship_mode()
 *
 * Enter ship mode (ultra-low power shutdown). Device can only be woken up
 * by hardware reset or power cycle. This is typically used for long-term
 * storage or shipping.
 *
 * WARNING: After calling this function, the device will be completely powered
 * down and will require a hardware reset to wake up!
 *
 * Example:
 *   frame.ship_mode()
 *
 * @param L Lua state
 * @return 0 (function does not return if successful)
 */
static int lua_ship_mode(lua_State *L)
{
	ARG_UNUSED(L);

	if (halo_battery_is_charging()) {
		return luaL_error(L, "cannot enter ship mode while charging");
	}

	const struct device *sm = DEVICE_DT_GET(DT_ALIAS(shutdown));

	if (!device_is_ready(sm)) {
		return luaL_error(L, "ship mode device not ready");
	}

	LOG_WRN("Entering ship mode - device will shut down completely");
	LOG_WRN("Hardware reset required to wake up!");

	/* Give time for log messages to be sent */
	k_sleep(K_MSEC(100));

	int ret = shutdown(sm);
	if (ret < 0) {
		return luaL_error(L, "failed to enter ship mode: %d", ret);
	}

	/* Should not reach here if ship mode works correctly */
	k_sleep(K_MSEC(100));
	LOG_ERR("WARNING: Still running after ship mode command!");

	return 0;
}

/**
 * @brief Lua: frame.get_se_revision()
 *
 * Get Secure Enclave firmware revision. This function performs the actual
 * service call to retrieve the revision, unlike the deprecated SE_REVISION
 * field which was loaded at initialization time.
 *
 * Example:
 *   local revision = frame.get_se_revision()
 *   print("SE Revision: " .. revision)
 *
 * @param L Lua state
 * @return 1 (SE revision string, or "unknown" on failure)
 */
static int lua_get_se_revision(lua_State *L)
{
	/* se_service_get_se_revision() memcpy()s resp_se_revision_length bytes
	 * (up to VERSION_RESPONSE_LENGTH, 80) and does NOT terminate them, so an
	 * exactly-80-byte buffer leaves lua_pushstring() scanning past the end
	 * into stack memory for its NUL. That appended whatever happened to be
	 * there to the revision string -- observed as a trailing 0xbb, which is
	 * also not valid UTF-8 and broke clients decoding the reply. Over-size by
	 * one and zero it, so the string is always terminated within the buffer.
	 * (se_mgmt.c does the equivalent with a 128-byte buffer and a memset.)
	 */
	uint8_t se_revision[81] = {0};
	int ret = se_service_get_se_revision(se_revision);

	if (ret == 0) {
		se_revision[sizeof(se_revision) - 1] = '\0';
		lua_pushstring(L, (const char *)se_revision);
	} else {
		LOG_WRN("Failed to get SE revision: %d", ret);
		lua_pushstring(L, "unknown");
	}

	return 1;
}

/**
 * @brief Lua: frame.charge(enable)
 *
 * Control battery charging enable/disable.
 *
 * Example:
 *   frame.charge(true)   -- Enable charging
 *   frame.charge(false)  -- Disable charging
 *
 * @param L Lua state
 * @return 0
 */
static int lua_charge(lua_State *L)
{
	int nargs = lua_gettop(L);
	bool enable = true; // Default to enable

	if (nargs >= 1) {
		enable = lua_toboolean(L, 1);
	}

	const struct device *dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(vbat));
	if (!dev) {
		return luaL_error(L, "VBAT device not found");
	}

	struct sensor_value val = {.val1 = enable ? 1 : 0, .val2 = 0};

	int ret = sensor_attr_set(dev, SENSOR_CHAN_ALL, SENSOR_ATTR_CONFIGURATION, &val);
	if (ret < 0) {
		return luaL_error(L, "Failed to set charge control: %d", ret);
	}

	return 0;
}

/**
 * @brief Lua: frame.wakeup_source()
 *
 * Get the source that woke the device from sleep.
 *
 * Returns a string representing the wakeup source:
 * - "unknown" - Unknown wakeup source
 * - "timeout" - Wakeup by timeout
 * - "button" - Wakeup by button press
 * - "ble" - Wakeup by BLE data
 * - "imu" - Wakeup by IMU interrupt
 * - "microphone" - Wakeup by microphone interrupt
 * - "external" - Wakeup by external interrupt
 * - "watchdog" - Wakeup by watchdog timer
 *
 * Example:
 *   local source = frame.wakeup_source()
 *   print("Woke up from: " .. source)
 *
 * @param L Lua state
 * @return 1 (string representing wakeup source)
 */
static int lua_wakeup_source(lua_State *L)
{
	halo_pm_wakeup_source_t source = halo_pm_get_wakeup_source();
	const char *source_name = halo_pm_wakeup_source_name(source);

	lua_pushstring(L, source_name);
	return 1;
}

/**
 * @brief Service lifecycle event handler
 *
 * Handles events like INTERRUPT (Ctrl+C) to reset state.
 */
static int system_service_event_handler(halo_lua_event_t event, void *user_data)
{
	ARG_UNUSED(user_data);

	switch (event) {
	case HALO_LUA_EVENT_INIT:
		sleep_interrupted = false;
		break;

	case HALO_LUA_EVENT_DEINIT:
		/* Set interrupt flag to break out of sleep */
		sleep_interrupted = true;
		break;

	case HALO_LUA_EVENT_INTERRUPT:
		/* Set interrupt flag to break out of sleep */
		sleep_interrupted = true;
		/* Reset state on Ctrl+C */
		break;

	default:
		break;
	}

	return 0;
}

/* Register service with lifecycle management (always-on service) */
HALO_LUA_SERVICE_DEFINE(system_service, system_service_event_handler, NULL, true);

/**
 * @brief Open and register system library with Lua VM
 *
 * Registers all system functions under the global 'frame' table.
 */
int lua_open_system_library(lua_State *L)
{
	LOG_DBG("Registering system library");

	stay_awake = false;

	/* Register service for lifecycle management */
	int ret = halo_lua_service_register(&system_service);
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("Failed to register system service: %d", ret);
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

	/* Register functions in frame table */
	lua_pushcfunction(L, lua_sleep);
	lua_setfield(L, -2, "sleep");

	lua_pushcfunction(L, lua_light_sleep);
	lua_setfield(L, -2, "light_sleep");

	lua_pushcfunction(L, lua_standby);
	lua_setfield(L, -2, "standby");

	lua_pushcfunction(L, lua_kyield);
	lua_setfield(L, -2, "yield");

	lua_pushcfunction(L, lua_reboot);
	lua_setfield(L, -2, "reboot");

	lua_pushcfunction(L, lua_stay_awake);
	lua_setfield(L, -2, "stay_awake");

	lua_pushcfunction(L, lua_battery_level);
	lua_setfield(L, -2, "battery_level");

	lua_pushcfunction(L, lua_battery_voltage);
	lua_setfield(L, -2, "battery_voltage");

	lua_pushcfunction(L, lua_battery_charging);
	lua_setfield(L, -2, "battery_charging");

	lua_pushcfunction(L, lua_ship_mode);
	lua_setfield(L, -2, "ship_mode");

	lua_pushcfunction(L, lua_get_se_revision);
	lua_setfield(L, -2, "get_se_revision");

	lua_pushcfunction(L, lua_charge);
	lua_setfield(L, -2, "charge");

	lua_pushcfunction(L, lua_wakeup_source);
	lua_setfield(L, -2, "wakeup_source");

	lua_pushcfunction(L, lua_get_eui);
	lua_setfield(L, -2, "get_eui");

	/* Pop frame table */
	lua_pop(L, 1);

	LOG_DBG("System library registered successfully");
	return 0;
}
