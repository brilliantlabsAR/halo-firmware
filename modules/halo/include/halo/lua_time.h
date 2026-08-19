/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_LUA_TIME_H
#define HALO_LUA_TIME_H

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file lua_time.h
 * @brief Lua time library for Halo platform
 * 
 * Provides time and date functionality to Lua scripts including:
 * - UTC time management
 * - Time zone support
 * - Date/time formatting and parsing
 * 
 * All functions are registered under frame.time table.
 */

/**
 * @brief Open and register the time library with Lua VM
 * 
 * This function registers all time functions under frame.time table.
 * It should be called during Lua VM initialization.
 * 
 * Example Lua usage:
 * @code{.lua}
 * -- Set UTC time (Unix timestamp)
 * frame.time.utc(1234567890)
 * 
 * -- Get current UTC time
 * local utc = frame.time.utc()
 * print("UTC time: " .. utc)
 * 
 * -- Set time zone
 * frame.time.zone("+08:00")  -- Beijing time
 * frame.time.zone("-05:00")  -- Eastern time
 * 
 * -- Get time zone
 * local tz = frame.time.zone()
 * print("Time zone: " .. tz)
 * 
 * -- Get local date/time as table
 * local date = frame.time.date()
 * print(string.format("%04d-%02d-%02d %02d:%02d:%02d",
 *     date.year, date.month, date.day,
 *     date.hour, date.minute, date.second))
 * 
 * -- Convert timestamp to date
 * local date = frame.time.date(1234567890)
 * @endcode
 * 
 * @param L Lua state
 * @return 0 on success
 */
int lua_open_time_library(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif /* HALO_LUA_TIME_H */
