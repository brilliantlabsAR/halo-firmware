/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_LUA_BUTTON_H
#define HALO_LUA_BUTTON_H

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file lua_button.h
 * @brief Lua Button library for Halo platform
 * 
 * Provides button input functionality to Lua scripts including:
 * - Single click detection
 * - Double click detection
 * - Long press detection
 * - User-defined callbacks for each button event
 * 
 * All functions are registered under frame.button table.
 */

/**
 * @brief Open and register the button library with Lua VM
 * 
 * This function registers all button functions under frame.button table.
 * It should be called during Lua VM initialization.
 * 
 * Example Lua usage:
 * @code{.lua}
 * -- Register single click callback
 * frame.button.single(function()
 *     print("Single click detected!")
 * end)
 * 
 * -- Register double click callback
 * frame.button.double(function()
 *     print("Double click detected!")
 * end)
 * 
 * -- Register long press callback
 * frame.button.long(function()
 *     print("Long press detected!")
 * end)
 * 
 * -- Clear a callback
 * frame.button.single(nil)
 * @endcode
 * 
 * @param L Lua state
 * @return 0 on success, negative error code on failure
 */
int lua_open_button_library(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif /* HALO_LUA_BUTTON_H */
