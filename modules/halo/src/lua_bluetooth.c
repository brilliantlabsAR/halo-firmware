/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "lua.h"
#include "lauxlib.h"
#include <halo/lua_bluetooth.h>
#include <halo/lua_service.h>
#include <halo/ble_manager.h>
#include <halo/ble_lua.h>


LOG_MODULE_REGISTER(lua_bluetooth, CONFIG_HALO_LOG_LEVEL);

/**
 * @brief Bluetooth callback state
 *
 * Stores the Lua callback reference and data for async interrupt handling.
 */
static struct {
	int callback_ref; /* LUA_REGISTRYINDEX reference */
	uint8_t data[CONFIG_HALO_LUA_MAX_DATA_SIZE];
	size_t length;
	bool has_data;
} bt_callback_state = {
	.callback_ref = LUA_NOREF,
	.length = 0,
	.has_data = false,
};

/**
 * @brief Lua hook handler for data interrupt
 *
 * Called asynchronously when BLE data is received.
 * Executes the registered Lua callback with received data.
 */
static void bluetooth_receive_hook_handler(lua_State *L, lua_Debug *ar)
{
	ARG_UNUSED(ar);

	/* Clear the hook immediately */
	lua_sethook(L, NULL, 0, 0);

	if (bt_callback_state.callback_ref == LUA_NOREF) {
		LOG_WRN("Data interrupt but no callback registered");
		bt_callback_state.has_data = false;
		return;
	}

	if (!bt_callback_state.has_data) {
		LOG_WRN("Hook called but no data available");
		return;
	}

	/* Get callback function from registry */
	lua_rawgeti(L, LUA_REGISTRYINDEX, bt_callback_state.callback_ref);

	/* Push data as string argument */
	lua_pushlstring(L, (const char *)bt_callback_state.data, bt_callback_state.length);

	/* Clear data flag */
	bt_callback_state.has_data = false;

	/* Call callback function */
	if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
		const char *error = lua_tostring(L, -1);
		LOG_ERR("Bluetooth callback error: %s", error);
		lua_pop(L, 1);
	}
}

/**
 * @brief Handle incoming BLE data (called from lua_runtime data thread)
 *
 * @param L Lua state
 * @param data Received data buffer
 * @param length Data length
 */
void lua_bluetooth_data_interrupt(lua_State *L, const uint8_t *data, size_t length)
{
	if (bt_callback_state.callback_ref == LUA_NOREF) {
		return;
	}

	if (length > sizeof(bt_callback_state.data)) {
		LOG_ERR("Data too large: %u > %u", length, sizeof(bt_callback_state.data));
		length = sizeof(bt_callback_state.data);
	}

	/* Copy data to callback state */
	memcpy(bt_callback_state.data, data, length);
	bt_callback_state.length = length;
	bt_callback_state.has_data = true;

	/* Set hook to trigger callback on next Lua instruction */
	lua_sethook(L, bluetooth_receive_hook_handler,
		    LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE | LUA_MASKCOUNT, 1);
}

/**
 * @brief Lua: frame.bluetooth.is_connected()
 *
 * Check if BLE is connected.
 *
 * @param L Lua state
 * @return 1 (boolean: true if connected)
 */
static int lua_bluetooth_is_connected(lua_State *L)
{
	bool connected = halo_ble_is_connected();
	lua_pushboolean(L, connected);
	return 1;
}

/**
 * @brief Lua: frame.bluetooth.address()
 *
 * Get device MAC address as string.
 *
 * @param L Lua state
 * @return 1 (string: MAC address in format "XX:XX:XX:XX:XX:XX")
 */
static int lua_bluetooth_address(lua_State *L)
{
	uint8_t addr[6];
	int ret = halo_ble_get_address(addr);

	if (ret < 0) {
		LOG_ERR("Failed to get BLE address: %d", ret);
		lua_pushstring(L, "00:00:00:00:00:00");
		return 1;
	}

	char addr_str[18];
	snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X", addr[5], addr[4],
		 addr[3], addr[2], addr[1], addr[0]);

	LOG_DBG("Bluetooth address: %s", addr_str);
	lua_pushstring(L, addr_str);
	return 1;
}

/**
 * @brief Lua: frame.bluetooth.max_length()
 *
 * Get maximum data length for send() operation.
 * Returns MTU size minus 1 byte (for internal marker).
 *
 * @param L Lua state
 * @return 1 (number: max data length in bytes)
 */
static int lua_bluetooth_max_length(lua_State *L)
{
	uint16_t mtu = halo_ble_get_mtu();

	/* Subtract 1 for data marker byte that we add in send() */
	int max_len = mtu - 1;
	if (max_len < 0) {
		max_len = 0;
	}

	lua_pushinteger(L, max_len);
	return 1;
}

/**
 * @brief Lua: frame.bluetooth.send(data)
 *
 * Send data over BLE Data channel.
 * Data is prefixed with a marker byte before transmission.
 * Supports fragmentation for large data packets.
 *
 * @param L Lua state
 * @return 0 on success, error on failure
 */
static int lua_bluetooth_send(lua_State *L)
{
	size_t length;
	const char *data = luaL_checklstring(L, 1, &length);

	/* Check if connected */
	if (!halo_ble_is_connected()) {
		return luaL_error(L, "not connected");
	}

	/* Get max length per packet (MTU - 1 for marker). halo_ble_get_mtu()
	 * returns 0 when the MTU is not yet known; a size_t (mtu - 1) would then
	 * underflow to SIZE_MAX and defeat an unsigned guard, so check signed and
	 * bound the on-stack chunk buffer to the maximum ATT payload. */
	int mtu = halo_ble_get_mtu();
	int max_len = mtu - 1;

	if (max_len <= 0) {
		return luaL_error(L, "invalid MTU size");
	}
	/* Hard cap the on-stack chunk buffer at the maximum LE ATT payload,
	 * independent of whatever MTU is reported. */
	if (max_len > 512) {
		max_len = 512;
	}

	/* Send data in chunks if necessary */
	size_t remaining = length;
	size_t offset = 0;

	while (remaining > 0) {
		size_t chunk_size = (remaining > (size_t)max_len) ? (size_t)max_len : remaining;

		/* Prepare buffer with marker byte */
		uint8_t buffer[chunk_size + 1];
		buffer[0] = HALO_LUA_CTRL_DATA_MARKER; /* Data marker */
		memcpy(buffer + 1, data + offset, chunk_size);

		/* Write to BLE Data characteristic */
		int32_t written = halo_ble_lua_data_write(buffer, chunk_size + 1);

		if (written != chunk_size + 1) {
			k_yield();
			return luaL_error(L, "send failed: %d of %u bytes written at offset %u",
			                 written, (unsigned int)(chunk_size + 1), (unsigned int)offset);
		}

		offset += chunk_size;
		remaining -= chunk_size;
	}
	return 0;
}

/**
 * @brief Lua: frame.bluetooth.receive_callback(function)
 *
 * Register or clear data receive callback.
 * - Pass function to register callback
 * - Pass nil to clear callback
 *
 * @param L Lua state
 * @return 0
 */
static int lua_bluetooth_receive_callback(lua_State *L)
{
	/* Check if clearing callback (nil argument) */
	if (lua_isnil(L, 1)) {
		if (bt_callback_state.callback_ref != LUA_NOREF) {
			luaL_unref(L, LUA_REGISTRYINDEX, bt_callback_state.callback_ref);
			bt_callback_state.callback_ref = LUA_NOREF;
			bt_callback_state.has_data = false;
		}
		LOG_DBG("Bluetooth receive_callback cleared");
		return 0;
	}

	/* Must be a function */
	if (!lua_isfunction(L, 1)) {
		return luaL_error(L, "expected function or nil");
	}

	/* Unref old callback if exists */
	if (bt_callback_state.callback_ref != LUA_NOREF) {
		luaL_unref(L, LUA_REGISTRYINDEX, bt_callback_state.callback_ref);
	}

	/* Store new callback in registry */
	bt_callback_state.callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	bt_callback_state.has_data = false;

	LOG_DBG("Bluetooth receive_callback registered");
	return 0;
}

/**
 * @brief Service lifecycle event handler
 *
 * Handles lifecycle events like INTERRUPT (Ctrl+C).
 */
static int bluetooth_service_event_handler(halo_lua_event_t event, void *user_data)
{
	ARG_UNUSED(user_data);

	switch (event) {
	case HALO_LUA_EVENT_INIT:
		bt_callback_state.callback_ref = LUA_NOREF;
		bt_callback_state.has_data = false;
		break;

	case HALO_LUA_EVENT_DEINIT:
		/* Callback will be cleaned up by Lua GC */
		bt_callback_state.callback_ref = LUA_NOREF;
		bt_callback_state.has_data = false;
		break;

	case HALO_LUA_EVENT_INTERRUPT:
		/* Clear callback on Ctrl+C */
		bt_callback_state.callback_ref = LUA_NOREF;
		bt_callback_state.has_data = false;
		break;

	default:
		break;
	}

	return 0;
}

/* Register service with lifecycle management (always-on service) */
HALO_LUA_SERVICE_DEFINE(bluetooth_service, bluetooth_service_event_handler, NULL, true);

/**
 * @brief Open and register bluetooth library with Lua VM
 *
 * Registers all bluetooth functions under frame.bluetooth table.
 */
int lua_open_bluetooth_library(lua_State *L)
{
	/* Register service for lifecycle management */
	int ret = halo_lua_service_register(&bluetooth_service);
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("Failed to register bluetooth service: %d", ret);
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

	/* Create 'bluetooth' subtable */
	lua_newtable(L);

	/* Register bluetooth functions */
	lua_pushcfunction(L, lua_bluetooth_is_connected);
	lua_setfield(L, -2, "is_connected");

	lua_pushcfunction(L, lua_bluetooth_address);
	lua_setfield(L, -2, "address");

	lua_pushcfunction(L, lua_bluetooth_max_length);
	lua_setfield(L, -2, "max_length");

	lua_pushcfunction(L, lua_bluetooth_send);
	lua_setfield(L, -2, "send");

	lua_pushcfunction(L, lua_bluetooth_receive_callback);
	lua_setfield(L, -2, "receive_callback");

	/* Set 'bluetooth' table in 'frame' */
	lua_setfield(L, -2, "bluetooth");

	/* Pop frame table */
	lua_pop(L, 1);

	LOG_DBG("Bluetooth library registered successfully");
	return 0;
}
