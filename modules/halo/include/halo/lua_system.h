/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_LUA_SYSTEM_H
#define HALO_LUA_SYSTEM_H

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file lua_system.h
 * @brief Lua system library for Halo platform
 * 
 * Provides system-level functionality to Lua scripts including:
 * - Sleep and power management
 * - Battery status monitoring
 * - System control functions
 * 
 * All functions are registered under the global 'frame' table for
 * compatibility with existing Lua scripts.
 */

/**
 * @brief Open and register the system library with Lua VM
 * 
 * This function registers all system functions under the 'frame' global table.
 * It should be called during Lua VM initialization.
 * 
 * Example Lua usage:
 * @code{.lua}
 * -- Sleep for 1 second
 * frame.sleep(1.0)
 * 
 * -- Get battery level
 * local level = frame.battery_level()
 * print("Battery: " .. level .. "%")
 * 
 * -- Check charging status
 * if frame.battery_charging() then
 *     print("Charging")
 * end
 * 
 * -- Stay awake mode
 * frame.stay_awake(true)  -- Prevent auto-sleep
 * frame.stay_awake(false) -- Allow auto-sleep
 * local is_awake = frame.stay_awake() -- Get current state
 * 
 * -- Get EUI-64 address
 * local eui = frame.get_eui() -- Returns 16-character hex string (e.g., "2CF7F1AABBCCDDEE")
 * @endcode
 * 
 * @param L Lua state
 * @return 0 on success
 */
int lua_open_system_library(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif /* HALO_LUA_SYSTEM_H */
