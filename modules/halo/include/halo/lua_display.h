/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_LUA_DISPLAY_H
#define HALO_LUA_DISPLAY_H

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file lua_display.h
 * @brief Lua Display library for Halo platform
 * 
 * Provides comprehensive display functionality to Lua scripts including:
 * - Bitmap rendering with multiple color formats
 * - Text rendering with multiple fonts
 * - Geometric shapes (pixels, lines, rectangles, circles, polygons)
 * - Color palette management
 * - Power management
 * 
 * All functions are registered under frame.display table.
 */

/**
 * @brief Open and register the display library with Lua VM
 * 
 * This function registers all display functions under frame.display table and
 * initializes the display hardware (panel, DSI, CDC200 if available).
 * It should be called during Lua VM initialization.
 * 
 * Example Lua usage:
 * @code{.lua}
 * -- Initialize and clear display
 * frame.display.clear(0x000000)  -- Clear to black
 * 
 * -- Set global palette colors
 * frame.display.assign_color(1, 255, 0, 0)    -- Red
 * frame.display.assign_color(2, 0, 255, 0)    -- Green
 * frame.display.assign_color(3, 0, 0, 255)    -- Blue
 * 
 * -- Draw text
 * frame.display.text("Hello World", 10, 10, 0xFFFFFF)
 * 
 * -- Draw bitmap (4-bit indexed color)
 * local bitmap_data = string.char(0x12, 0x34, ...)
 * frame.display.bitmap(0, 0, 16, 16, bitmap_data, {
 *     x_scale = 2,
 *     y_scale = 2
 * })
 * 
 * -- Draw shapes
 * frame.display.set_pixel(100, 100, 0xFF0000)
 * frame.display.line(0, 0, 100, 100, 0x00FF00)
 * frame.display.rect(50, 50, 100, 50, 0x0000FF, true)
 * frame.display.circle(160, 120, 30, 0xFFFF00, false)
 * 
 * -- Font management
 * local fonts = frame.display.get_font_list()
 * frame.display.set_font(0, 16, 1)  -- Dogica, 16px, scale 1 (sizes are multiples of 8)
 * 
 * -- Power management
 * frame.display.set_brightness(128)
 * frame.display.power_save(true)   -- Enter power save mode
 * @endcode
 * 
 * @param L Lua state
 * @return 0 on success, negative error code on failure
 */
int lua_open_display_library(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif /* HALO_LUA_DISPLAY_H */
