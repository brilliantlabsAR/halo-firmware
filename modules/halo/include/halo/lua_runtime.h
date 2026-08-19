/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_LUA_RUNTIME_H_
#define HALO_LUA_RUNTIME_H_

#include <zephyr/kernel.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/**
 * @brief Initialize Lua runtime
 * 
 * Creates REPL and data handler threads, initializes Lua VM.
 * Must be called after halo_ble_lua_init().
 * 
 * @return 0 on success, negative errno on error
 */
int halo_lua_runtime_init(void);

/**
 * @brief Deinitialize Lua runtime
 * 
 * Stops threads and cleans up resources.
 * 
 * @return 0 on success, negative errno on error
 */
int halo_lua_runtime_deinit(void);

/**
 * @brief Check if Lua runtime is running
 * 
 * @return true if runtime is active, false otherwise
 */
bool halo_lua_is_running(void);

/**
 * @brief Get current Lua state
 * 
 * @return Pointer to lua_State, or NULL if not initialized
 */
lua_State *halo_lua_get_state(void);

/**
 * @brief Interrupt Lua execution (Ctrl+C)
 * 
 * Triggers interrupt signal to stop current script execution.
 * Notifies all registered services via HALO_LUA_EVENT_INTERRUPT.
 */
void halo_lua_runtime_interrupt(void);

/**
 * @brief Restart Lua VM (Ctrl+D)
 * 
 * Closes current VM and restarts with fresh state.
 * Notifies all registered services via HALO_LUA_EVENT_RESTART.
 */
void halo_lua_runtime_restart(void);

/**
 * @brief Reset Lua VM and remove main.lua (Ctrl+Z)
 * 
 * Similar to restart, but also deletes main.lua from filesystem.
 * Notifies all registered services via HALO_LUA_EVENT_RESTART.
 */
void halo_lua_runtime_reset(void);

/**
 * @brief Exit Lua runtime completely
 * 
 * Stops the Lua VM and exits the REPL loop without restarting.
 * The runtime will need to be re-initialized to use again.
 * Notifies all registered services via HALO_LUA_EVENT_DEINIT.
 */
void halo_lua_runtime_exit(void);

/**
 * @brief Signal button event to Lua runtime
 * 
 * Called from button ISR to queue button event for processing
 * in the REPL thread context.
 * 
 * @param action Button action that occurred
 */
void halo_lua_runtime_button_event(int action);

#endif /* HALO_LUA_RUNTIME_H_ */
