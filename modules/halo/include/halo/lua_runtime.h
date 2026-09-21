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
 * Raises a single "interrupted" error in the running chunk on its next
 * VM instruction. A pcall can catch it and run to completion; nothing
 * else is reset, so callbacks registered from Lua stay registered.
 * Notifies all registered services via HALO_LUA_EVENT_INTERRUPT so
 * blocking calls return and hardware streams stop.
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
 * @brief Asynchronous event sources delivered on the Lua thread
 *
 * The Lua VM is single-threaded: every callback into Lua must run on the
 * REPL thread. Producers on other threads (BLE host, button driver, sensor
 * work, T5838 AAD work) queue their event in their own module, then call
 * halo_lua_event_signal(). The runtime owns Lua's single debug-hook slot:
 * the signal arms one shared hook, and when it fires on the REPL thread it
 * drains every source with a pending bit set, in the order below, so
 * sources can never clobber each other's hook and no event is lost.
 */
enum halo_lua_event_source {
	HALO_LUA_EVENT_SRC_BLE_DATA = 0, /* frame.bluetooth.receive_callback */
	HALO_LUA_EVENT_SRC_BUTTON,       /* frame.button.* callbacks */
	HALO_LUA_EVENT_SRC_IMU,          /* frame.imu.tap_callback */
	HALO_LUA_EVENT_SRC_MIC_AAD,      /* frame.microphone.aad_callback */
	HALO_LUA_EVENT_SRC_ANCS,         /* frame.ancs.* callbacks */
	HALO_LUA_EVENT_SRC_COUNT,
};

/**
 * @brief Drain function for one event source
 *
 * Runs on the REPL thread inside the shared hook with the VM available.
 * Must consume every queued event for its source (the pending bit was
 * cleared before the call) and must not call lua_sethook().
 */
typedef void (*halo_lua_event_drain_t)(lua_State *L);

/**
 * @brief Register the drain function for an event source
 *
 * Idempotent; call from the module's library-open or init path.
 */
void halo_lua_event_drain_set(enum halo_lua_event_source src, halo_lua_event_drain_t drain);

/**
 * @brief Mark an event source pending and arm the Lua hook
 *
 * Safe to call from any thread. A no-op when the VM is not running - the
 * source keeps its own queue, so it can decide whether to buffer or drop.
 */
void halo_lua_event_signal(enum halo_lua_event_source src);

#endif /* HALO_LUA_RUNTIME_H_ */
