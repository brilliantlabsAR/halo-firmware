/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_LUA_BLUETOOTH_H
#define HALO_LUA_BLUETOOTH_H

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file lua_bluetooth.h
 * @brief Lua Bluetooth library for Halo platform
 * 
 * Provides Bluetooth functionality to Lua scripts including:
 * - Connection status monitoring
 * - Data transmission over BLE Lua Data channel
 * - Connection information (MTU, address)
 * - Data receive callbacks
 * 
 * All functions are registered under frame.bluetooth table.
 */

/**
 * @brief Open and register the bluetooth library with Lua VM
 * 
 * This function registers all bluetooth functions under frame.bluetooth table.
 * It should be called during Lua VM initialization.
 * 
 * Example Lua usage:
 * @code{.lua}
 * -- Check connection status
 * if frame.bluetooth.is_connected() then
 *     print("BLE connected")
 * end
 * 
 * -- Get MAC address
 * local addr = frame.bluetooth.address()
 * print("Address: " .. addr)
 * 
 * -- Get max data length
 * local max = frame.bluetooth.max_length()
 * print("Max data: " .. max .. " bytes")
 * 
 * -- Send data over BLE Data channel
 * frame.bluetooth.send("Hello from Lua!")
 * 
 * -- Register receive callback
 * frame.bluetooth.receive_callback(function(data)
 *     print("Received: " .. data)
 * end)
 * 
 * -- Clear callback
 * frame.bluetooth.receive_callback(nil)
 * @endcode
 * 
 * @param L Lua state
 * @return 0 on success
 */
int lua_open_bluetooth_library(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif /* HALO_LUA_BLUETOOTH_H */
