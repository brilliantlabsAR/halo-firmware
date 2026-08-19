/*
 * Copyright (c) 2024 Alif Semiconductor
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <halo/ble_lua.h>

/**
 * @file luaport.h
 * @brief Lua port macros for Halo platform
 * 
 * These macros are used by Lua's print() function and error reporting
 * to redirect output to the BLE REPL characteristic instead of stdout.
 * 
 * Lua expects these macros to be defined in luaport.h to customize
 * output behavior for embedded systems.
 */

/**
 * Write string to BLE REPL output
 * Used by Lua's print() function
 * 
 * @param s Pointer to string
 * @param l Length of string
 */
#define lua_writestring(s, l) halo_ble_lua_repl_write((const uint8_t *)(s), (l))

/**
 * Write newline to BLE REPL output
 * Called after print() completes
 * Note: Empty implementation like frame module - newlines are handled by lua_writestring
 */
#define lua_writeline()

/**
 * Write error string with printf-style formatting
 * Used for panic messages and error reporting
 * Output to system log via printk (same as frame module)
 * 
 * @param s Format string
 * @param p Format argument
 */
#define lua_writestringerror(s, p) printk(s, p)
