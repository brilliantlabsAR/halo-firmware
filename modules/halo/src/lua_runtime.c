/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>

#include <app_version.h>

#include "halo/lua_runtime.h"
#include "halo/lua_service.h"
#include "halo/lua_version.h"
#include "halo/lua_system.h"
#include "halo/lua_time.h"
#include "halo/lua_file.h"
#include "halo/lua_bluetooth.h"
#include "halo/lua_compression.h"
#include "halo/lua_button.h"
#include "halo/lua_imu.h"
#include "halo/lua_log.h"
#include "halo/lua_display.h"
#include "halo/lua_camera.h"
#include "halo/lua_microphone.h"
#include "halo/lua_speaker.h"
#include "halo/lua_sound.h"
#ifdef CONFIG_HALO_BLE_ANCS_CLIENT
#include "halo/lua_ancs.h"
#endif
#ifdef CONFIG_HALO_BLE_MANAGER
#include "halo/ble_connection.h"
#endif

#include "halo/pm_manager.h"
#include "halo/ble_lua.h"
#include "halo/file_manager.h"
#include "halo/mem_manager.h"

LOG_MODULE_REGISTER(halo_lua_runtime, CONFIG_HALO_LOG_LEVEL);

/**
 * @brief Custom Lua allocator using Halo memory manager
 *
 * This allocator is called by Lua for all memory operations:
 * - malloc: osize=0, nsize>0
 * - free:   osize>0, nsize=0
 * - realloc: osize>0, nsize>0
 *
 * @param ud User data (unused)
 * @param ptr Pointer to existing block (NULL for malloc)
 * @param osize Original size
 * @param nsize New size
 * @return Pointer to allocated memory, or NULL on failure
 */
static void *lua_halo_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
	ARG_UNUSED(ud);
	ARG_UNUSED(osize); /* We trust Lua to provide correct osize */

	if (nsize == 0) {
		/* Free */
		if (ptr) {
			halo_free(ptr);
		}
		return NULL;
	} else {
		/* Allocate or reallocate */
		if (ptr == NULL) {
			/* New allocation */
			return halo_malloc(nsize, HALO_MEM_REGION_AUTO);
		} else {
			/* Reallocation */
			return halo_realloc(ptr, nsize, HALO_MEM_REGION_AUTO);
		}
	}
}

/* Control command handler for BLE */
static void lua_ctrl_handler(uint8_t ctrl_code)
{
	switch (ctrl_code) {
	case HALO_LUA_CTRL_REBOOT:
		LOG_INF("Rebooting system via Ctrl+B");
#ifdef CONFIG_HALO_BLE_MANAGER
		halo_ble_conn_prepare_reboot();
#endif
		sys_reboot(SYS_REBOOT_COLD);
		break;
	case HALO_LUA_CTRL_INTERRUPT:
		halo_lua_runtime_interrupt();
		break;
	case HALO_LUA_CTRL_RESTART:
		halo_lua_runtime_restart();
		break;
	case HALO_LUA_CTRL_RESET:
		halo_lua_runtime_reset();
		break;
	case HALO_LUA_CTRL_EXIT:
		halo_lua_runtime_exit();
		break;
	case HALO_LUA_CTRL_REMOVE_ALL:
		LOG_INF("Removing all files via Ctrl+G");
		halo_file_remove_all();
		break;
	default:
		LOG_WRN("Unknown control code: 0x%02X", ctrl_code);
		break;
	}
}

/* Lua runtime context */
static struct {
	bool initialized;
	bool running;
	bool exited; /* Flag to indicate complete exit (not restart) */
	bool interrupted; /* Flag for Ctrl+C interrupt */
	bool standby_hold; /* Standby: pause the VM in place until wake */
	lua_State *L;

	/* Guards L against the VM being torn down while another thread arms
	 * the hook (lua_hook_arm vs lua_vm_cleanup). Held only for the few
	 * instructions of lua_sethook / the pointer swap. */
	struct k_spinlock hook_lock;

	/* Async event dispatch (see halo_lua_event_signal) */
	atomic_t pending; /* bitmask of enum halo_lua_event_source */
	halo_lua_event_drain_t drains[HALO_LUA_EVENT_SRC_COUNT];

	/* Threads */
	struct k_thread repl_thread;

	/* Thread start synchronization */
	struct k_sem repl_thread_started;

	/* Thread exit synchronization - for PM to wait for REPL cleanup */
	struct k_sem repl_exited_sem;

#ifdef CONFIG_HALO_PM_MANAGER
	/* Power management */
	struct halo_pm_callback pm_cb;
#endif

	/* Buffers */
	uint8_t repl_buffer[CONFIG_HALO_LUA_MAX_REPL_SIZE];
	size_t repl_len;
	uint8_t line_buffer[CONFIG_HALO_LUA_MAX_REPL_SIZE];

} lua_ctx = {
	.initialized = false,
	.running = false,
	.exited = false,
	.interrupted = false,
	.standby_hold = false,
	.L = NULL,
	.pending = ATOMIC_INIT(0),
};

/* Thread stacks */
K_THREAD_STACK_DEFINE(lua_repl_stack, CONFIG_HALO_LUA_REPL_TASK_STACK_SIZE);

/* Forward declarations */
static void lua_repl_thread_fn(void *p1, void *p2, void *p3);
static void lua_vm_init(lua_State *L);
static void lua_vm_cleanup(void);

/*
 * Shared Lua hook.
 *
 * Lua has exactly one debug-hook slot per state, and lua_sethook() is the
 * only VM call that is safe from another thread (ldebug.c documents it as
 * signal-safe). Every asynchronous need - Ctrl+C / restart / exit breaks,
 * standby pause, and the module callbacks (BLE data, button, IMU tap, AAD,
 * ANCS) - therefore funnels through this one handler, which runs on the
 * REPL thread on the next VM instruction. Producers never touch the slot
 * directly; they set state and call lua_hook_arm().
 */
static void lua_hook_dispatch(lua_State *L, lua_Debug *ar)
{
	ARG_UNUSED(ar);

	/* Break request (Ctrl+C, restart, exit, light sleep): unwind the
	 * running chunk. The hook stays armed so a break raised while the
	 * REPL is between commands still trips the next chunk. Pending
	 * module events are kept; they drain once the flag clears. */
	if (lua_ctx.interrupted || !lua_ctx.running) {
		luaL_error(L, "interrupted");
	}

	/* Disarm before reading pending bits: a signal that lands after
	 * this point re-arms the hook, so it fires again for anything the
	 * loop below misses. */
	lua_sethook(L, NULL, 0, 0);

	/* Standby: hold the VM in place; execution resumes at this
	 * statement on wake. */
	if (lua_ctx.standby_hold) {
		while (halo_pm_is_sleeping()) {
			k_sleep(K_MSEC(100));
		}
		lua_ctx.standby_hold = false;
	}

	uint32_t mask;
	while ((mask = (uint32_t)atomic_clear(&lua_ctx.pending)) != 0) {
		for (int src = 0; src < HALO_LUA_EVENT_SRC_COUNT; src++) {
			if ((mask & BIT(src)) && lua_ctx.drains[src]) {
				lua_ctx.drains[src](L);
			}
		}
	}
}

/* Arm the shared hook on the live VM (any thread). */
static void lua_hook_arm(void)
{
	k_spinlock_key_t key = k_spin_lock(&lua_ctx.hook_lock);

	if (lua_ctx.L) {
		lua_sethook(lua_ctx.L, lua_hook_dispatch,
			    LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE | LUA_MASKCOUNT, 1);
	}
	k_spin_unlock(&lua_ctx.hook_lock, key);
}

void halo_lua_event_drain_set(enum halo_lua_event_source src, halo_lua_event_drain_t drain)
{
	if (src < HALO_LUA_EVENT_SRC_COUNT) {
		lua_ctx.drains[src] = drain;
	}
}

void halo_lua_event_signal(enum halo_lua_event_source src)
{
	if (src >= HALO_LUA_EVENT_SRC_COUNT) {
		return;
	}

	atomic_or(&lua_ctx.pending, BIT(src));

	if (lua_ctx.running) {
		lua_hook_arm();
	}
}

#ifdef CONFIG_HALO_PM_MANAGER
/* PM callback handler for Lua runtime */
static int lua_pm_callback_handler(halo_pm_event_t event, halo_pm_sleep_mode_t mode,
				   void *user_data)
{
	ARG_UNUSED(user_data);

	LOG_DBG("PM callback: event %d, mode %d", event, mode);

	if (event == HALO_PM_EVENT_SUSPEND) {
		if (mode == HALO_PM_SLEEP_DEEP) {
			/* Deep sleep: full shutdown — system resets on wake. */
			LOG_DBG("PM suspend DEEP: notifying services + exit");
			int ret = halo_lua_service_notify(HALO_LUA_EVENT_SUSPEND);
			if (ret < 0) {
				LOG_ERR("Lua service suspend failed: %d", ret);
				return ret;
			}
			halo_lua_runtime_exit();
			return 0;
		}

		if (mode == HALO_PM_SLEEP_LIGHT) {
			/* Light sleep: suspend peripherals, then let the calling
			 * thread block in k_sleep.  On wake the break-hook will
			 * throw "interrupted", which propagates out of
			 * require('main'), and the REPL outer-loop restarts the
			 * VM (main.lua runs from the top again).
			 * Do NOT call halo_lua_runtime_exit() — that would set
			 * exited=true and kill the REPL thread permanently. */
			LOG_DBG("PM suspend LIGHT: notifying services, preparing restart");
			int ret = halo_lua_service_notify(HALO_LUA_EVENT_SUSPEND);
			if (ret < 0) {
				LOG_ERR("Lua service suspend failed: %d", ret);
				return ret;
			}
			/* Prepare to break out of current Lua execution after wake */
			lua_ctx.running = false;
			lua_ctx.interrupted = true;
			lua_hook_arm();
			return 0;
		}

		/* Standby: pause Lua in-place (resume from same statement) */
		int ret = halo_lua_service_notify(HALO_LUA_EVENT_SUSPEND);
		if (ret < 0) {
			LOG_ERR("Lua service suspend failed: %d", ret);
			return ret;
		}
		if (lua_ctx.L && lua_ctx.running) {
			lua_ctx.standby_hold = true;
			lua_hook_arm();
			return 0; /* Hook installed (needs removal on resume) */
		}
		return 1; /* Skip if Lua not running */
	}

	if (event == HALO_PM_EVENT_RESUME) {
		if (mode == HALO_PM_SLEEP_LIGHT) {
			/* Light sleep: VM will be torn down and recreated by the
			 * REPL outer-loop.  Don't resume services — they'll go
			 * through DEINIT → INIT when the new VM starts. */
			LOG_DBG("PM resume LIGHT: skip service resume (REPL restarts VM)");
			return 0;
		}

		/* Standby / other: release the held VM and resume services.
		 * The hook itself is left alone - module events that queued
		 * during standby are still pending on it. */
		lua_ctx.standby_hold = false;
		halo_lua_service_notify(HALO_LUA_EVENT_RESUME);
		return 0;
	}

	return 1; /* Skip by default */
}
#endif

/* Initialize Lua VM and load libraries */
static void lua_vm_init(lua_State *L)
{

	/* Load standard libraries */
	luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
	luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
	luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
	luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
	luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
	luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
	luaL_requiref(L, LUA_DBLIBNAME, luaopen_debug, 1);
	lua_pop(L, 7);

	/* Create global 'frame' table for compatibility
	 * Note: Internal implementation uses halo architecture,
	 * but Lua API keeps 'frame' namespace for backward compatibility
	 */
	lua_newtable(L);
	lua_setglobal(L, "frame");

	/* Load version info (includes SE revision) */
	halo_lua_load_version_info(L);

	/* Load Halo Lua libraries (implemented under 'frame' namespace for compatibility)
	 * These will populate the 'frame' global table:
	 *   frame.sleep(), frame.battery_level(), etc.
	 *   frame.time.utc(), frame.time.zone(), frame.time.date()
	 *   frame.file.open(), frame.file.listdir(), etc.
	 *   frame.bluetooth.send(), frame.bluetooth.receive_callback(), etc.
	 *   frame.compression.decompress(), frame.compression.process_function()
	 *   frame.camera.*
	 *   frame.display.*
	 *   frame.imu.*
	 *   frame.microphone.*
	 */
	lua_open_system_library(L);
	lua_open_time_library(L);
	lua_open_file_library(L);
	lua_open_bluetooth_library(L);
	lua_open_compression_library(L);
	lua_open_button_library(L);
	lua_open_imu_library(L);
#if defined(CONFIG_LOG_BACKEND_FS)
	lua_open_log_library(L);
#endif
#if defined(CONFIG_DISPLAY)
	lua_open_display_library(L);
#endif
#if defined(CONFIG_VIDEO)
	lua_open_camera_library(L);
#endif

#if defined(CONFIG_HALO_LUA_MICROPHONE)
	lua_open_microphone_library(L);
#endif

#if defined(CONFIG_HALO_LUA_SPEAKER)
	lua_open_speaker_library(L);
#endif

#if defined(CONFIG_HALO_LUA_SOUND)
	lua_open_sound_library(L);
#endif

#if defined(CONFIG_HALO_BLE_ANCS_CLIENT)
	lua_open_ancs_library(L);
#endif

	/* Alias 'halo' to the same table as 'frame' so halo.* and frame.*
	 * refer to identical functions (e.g. halo.standby(), halo.display.text()).
	 */
	lua_getglobal(L, "frame");
	lua_setglobal(L, "halo");

	/* Notify services: VM initialized */
	halo_lua_service_notify(HALO_LUA_EVENT_INIT);

	/* Log memory usage after initialization */
	size_t lua_mem_kb = lua_gc(L, LUA_GCCOUNT, 0);
	size_t lua_mem_bytes = lua_gc(L, LUA_GCCOUNTB, 0);
	LOG_INF("Lua VM memory: %u.%03zu KB", lua_mem_kb, lua_mem_bytes);

	/* Load main.lua if exists */
	int status = luaL_dostring(L, "require('main')");
	if (status != LUA_OK) {
		const char *lua_error = lua_tostring(L, -1);
		LOG_ERR("Failed to load main.lua: %s", lua_error);

		/* Only print error if it's not just "file doesn't exist" */
		if (strcmp(lua_error,
			   "[string \"require('main')\"]:1: cannot open file: main.lua") != 0) {
			/* Send error to BLE */
			halo_ble_lua_repl_write((const uint8_t *)lua_error, strlen(lua_error));
			halo_ble_lua_repl_write((const uint8_t *)"\n", 1);
		}
		lua_pop(L, 1);
	}

	/* Log memory usage after loading main.lua */
	lua_mem_kb = lua_gc(L, LUA_GCCOUNT, 0);
	lua_mem_bytes = lua_gc(L, LUA_GCCOUNTB, 0);
}

/* Cleanup Lua VM */
static void lua_vm_cleanup(void)
{
	if (lua_ctx.L) {
		/* Notify services: VM shutting down */
		halo_lua_service_notify(HALO_LUA_EVENT_DEINIT);

		/* Detach the state under the hook lock so no producer can arm a
		 * hook on a state that is being closed, then close it. */
		k_spinlock_key_t key = k_spin_lock(&lua_ctx.hook_lock);
		lua_State *L = lua_ctx.L;

		lua_ctx.L = NULL;
		k_spin_unlock(&lua_ctx.hook_lock, key);

		lua_close(L);
		atomic_clear(&lua_ctx.pending);
	}
}

/* REPL thread - handles Lua command execution */
static void lua_repl_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/* Signal that thread has started */
	k_sem_give(&lua_ctx.repl_thread_started);

	while (!lua_ctx.exited) {
		LOG_DBG("REPL outer loop start exited=%d running=%d", lua_ctx.exited,
			lua_ctx.running);
		/* Reset buffers */
		lua_ctx.repl_len = 0;
		memset(lua_ctx.repl_buffer, 0, sizeof(lua_ctx.repl_buffer));

		/* Cleanup previous Lua state if exists */
		lua_vm_cleanup();

		/* Create new Lua state with custom allocator using Halo memory manager
		 * This ensures all Lua allocations go through halo_mem_manager:
		 * - Tables, strings, functions, closures
		 * - Internal VM structures
		 * - User data
		 * Benefits:
		 * - Uses external SRAM if configured
		 * - Unified memory statistics
		 * - Better control over memory usage
		 */
		lua_ctx.L = lua_newstate(lua_halo_alloc, NULL);
		if (!lua_ctx.L) {
			LOG_ERR("Failed to create Lua state!");
			k_sleep(K_SECONDS(1));
			continue;
		}

		/* Initialize VM */
		lua_ctx.running = true;
		lua_ctx.interrupted = false;
		lua_ctx.standby_hold = false;
		lua_vm_init(lua_ctx.L);

		/* REPL loop */
		while (lua_ctx.running) {
			/* Check available buffer space */
			size_t available = sizeof(lua_ctx.repl_buffer) - lua_ctx.repl_len;
			if (available == 0) {
				LOG_ERR("REPL buffer full! Resetting...");
				lua_ctx.repl_len = 0;
				available = sizeof(lua_ctx.repl_buffer);
			}

			/* Read from BLE REPL channel with longer timeout for efficiency */
		int32_t len = halo_ble_lua_repl_read(lua_ctx.repl_buffer + lua_ctx.repl_len,
							    available, K_MSEC(20));

			if (!lua_ctx.running || lua_ctx.exited) {
				LOG_DBG("REPL inner loop break running=%d exited=%d", lua_ctx.running,
					lua_ctx.exited);
				break;
			}

			lua_ctx.repl_len += len;

			if (len > 0) {
				/* Process complete lines */
				size_t start = 0;
				for (size_t i = 0; i < lua_ctx.repl_len; i++) {
					if (lua_ctx.repl_buffer[i] == '\n') {
						/* Found complete line */
						size_t line_len = i - start;

						/* Skip empty lines */
						if (line_len == 0) {
							start = i + 1;
							continue;
						}

						/* Copy line to buffer */
						memcpy(lua_ctx.line_buffer,
						       lua_ctx.repl_buffer + start, line_len);
						lua_ctx.line_buffer[line_len] = 0;
						start = i + 1;

						/* Check for control commands (defensive check) */
						if (line_len > 0 &&
						    (lua_ctx.line_buffer[0] ==
							     HALO_LUA_CTRL_INTERRUPT ||
						     lua_ctx.line_buffer[0] ==
							     HALO_LUA_CTRL_RESTART ||
						     lua_ctx.line_buffer[0] ==
							     HALO_LUA_CTRL_RESET ||
						     lua_ctx.line_buffer[0] ==
							     HALO_LUA_CTRL_EXIT ||
						     lua_ctx.line_buffer[0] ==
							     HALO_LUA_CTRL_REMOVE_ALL)) {
							LOG_WRN("REPL: Control char 0x%02X in line "
								"(should be filtered by BLE)",
								lua_ctx.line_buffer[0]);
							lua_ctx.running = false;
							break;
						}

						/* Record stack top before execution */
						int stack_top = lua_gettop(lua_ctx.L);
						
						/* Clear interrupt flag before executing new command
						 * Hook will check flag and auto-remove itself on next trigger */
						lua_ctx.interrupted = false;
						
						// printf("REPL executing: %s",
						// lua_ctx.line_buffer);
						/* Execute Lua code */
						int status = luaL_dostring(
							lua_ctx.L, (char *)lua_ctx.line_buffer);

						if (status != LUA_OK) {
							/* Handle error */
							const char *lua_error =
								lua_tostring(lua_ctx.L, -1);
							if (lua_error) {
								/* A chunk unwound by exit, restart or
								 * sleep is not a script error. */
								bool expected_shutdown_error =
									(lua_ctx.exited || !lua_ctx.running ||
									 halo_pm_is_sleeping());

								if (expected_shutdown_error) {
									LOG_DBG("Lua execution stopped during shutdown: %s",
										lua_error);
								} else {
									LOG_ERR("Lua error: %s", lua_error);
									halo_ble_lua_repl_write(
										(const uint8_t *)lua_error,
										strlen(lua_error));
									halo_ble_lua_repl_write(
										(const uint8_t *)"\n", 1);
								}
							}
							lua_pop(lua_ctx.L, 1);
						} else {
							/* Success - clean up any return values */
							int new_top = lua_gettop(lua_ctx.L);
							if (new_top > stack_top) {
								lua_pop(lua_ctx.L,
									new_top - stack_top);
							}
						}

						/* Verify stack is balanced */
						if (lua_gettop(lua_ctx.L) != stack_top) {
							LOG_WRN("Stack imbalance detected! "
								"Expected %d, got %d",
								stack_top, lua_gettop(lua_ctx.L));
							lua_settop(lua_ctx.L, stack_top);
						}
					}
				}

				if (!lua_ctx.running) {
					break;
				}

				/* Handle remaining data */
				if (start < lua_ctx.repl_len) {
					memmove(lua_ctx.repl_buffer, lua_ctx.repl_buffer + start,
						lua_ctx.repl_len - start);
					lua_ctx.repl_len -= start;
				} else {
					lua_ctx.repl_len = 0;
				}

				/* Check for buffer overflow risk */
				if (lua_ctx.repl_len > sizeof(lua_ctx.repl_buffer) * 3 / 4) {
					LOG_WRN("REPL buffer %u%% full - no newline found",
						lua_ctx.repl_len * 100 /
							sizeof(lua_ctx.repl_buffer));
				}
			} else if (lua_ctx.running && !lua_ctx.exited) {
				/* Idle: run a trivial chunk so the shared hook gets
				 * a VM instruction to fire on (pending callbacks,
				 * breaks). A break that lands with nothing running
				 * is consumed here rather than left to trip the
				 * next command. */
				int status = luaL_dostring(lua_ctx.L, "frame.yield()");
				if (status != LUA_OK) {
					lua_pop(lua_ctx.L, 1);
				}
				lua_ctx.interrupted = false;
			}
		}

		/* Cleanup VM */
		LOG_DBG("REPL cleanup begin");
		lua_vm_cleanup();
		LOG_DBG("REPL cleanup done, giving repl_exited_sem");

		/* Signal that REPL thread has exited the inner loop */
		/* This allows PM to wait for cleanup before entering deep sleep */
		k_sem_give(&lua_ctx.repl_exited_sem);
	}

	LOG_DBG("REPL thread exit final");
}

/* Public API implementations */

int halo_lua_runtime_init(void)
{
	if (lua_ctx.initialized) {
		return 0;
	}

	/* Initialize thread start semaphore */
	k_sem_init(&lua_ctx.repl_thread_started, 0, 1);

	/* Initialize thread exit synchronization semaphore */
	k_sem_init(&lua_ctx.repl_exited_sem, 0, 1);

	/* Register control command handler with BLE Lua service */
	halo_ble_lua_register_ctrl_handler(lua_ctrl_handler);

#ifdef CONFIG_HALO_PM_MANAGER
	/* Register PM callback for Lua sleep/wake control */
	int ret = halo_pm_register_callback(&lua_ctx.pm_cb, lua_pm_callback_handler, NULL,
					    "lua_runtime",
					    10); /* Priority 10 - lower than button/ble */
	if (ret != 0) {
		LOG_ERR("Failed to register PM callback: %d", ret);
		return ret;
	}
#endif

	/* Start REPL thread */
	k_thread_create(&lua_ctx.repl_thread, lua_repl_stack, K_THREAD_STACK_SIZEOF(lua_repl_stack),
			lua_repl_thread_fn, NULL, NULL, NULL, CONFIG_HALO_LUA_REPL_TASK_PRIORITY, 0,
			K_NO_WAIT);
	k_thread_name_set(&lua_ctx.repl_thread, "lua_repl");

	/* Wait for the thread to start */
	k_sem_take(&lua_ctx.repl_thread_started, K_FOREVER);

	lua_ctx.initialized = true;
	LOG_INF("Lua runtime initialized successfully");

	return 0;
}

int halo_lua_runtime_deinit(void)
{
	if (!lua_ctx.initialized) {
		return 0;
	}

	/* Stop running */
	lua_ctx.running = false;

	/* Cleanup VM if active */
	lua_vm_cleanup();

	/* Note: We don't abort threads here - they will restart VM */
	/* In a real production system, you'd want proper thread termination */

	lua_ctx.initialized = false;
	return 0;
}

bool halo_lua_is_running(void)
{
	return lua_ctx.running;
}

lua_State *halo_lua_get_state(void)
{
	return lua_ctx.L;
}

void halo_lua_runtime_interrupt(void)
{
	LOG_INF("Lua interrupt (Ctrl+C)");

	/* Set interrupt flag */
	lua_ctx.interrupted = true;

	/* Arm the hook so the running chunk unwinds */
	lua_hook_arm();

	/* Notify services first */
	halo_lua_service_notify(HALO_LUA_EVENT_INTERRUPT);
}

void halo_lua_runtime_restart(void)
{
	LOG_INF("Lua restart (Ctrl+D)");

	/* Stop current VM - REPL thread will restart it. The hook trips on
	 * !running, so a busy script is unwound rather than waited for. */
	lua_ctx.running = false;
	lua_hook_arm();
}

void halo_lua_runtime_reset(void)
{
	LOG_INF("Lua reset (Ctrl+E) - removing main.lua");

	/* Delete main.lua */
	const char *main_lua_path = "/lfs/main.lua";
	int ret = fs_unlink(main_lua_path);
	if (ret == 0) {
		LOG_INF("Deleted main.lua");
	} else {
		LOG_WRN("Failed to delete main.lua: %d", ret);
	}

	/* Restart VM */
	halo_lua_runtime_restart();
}

void halo_lua_runtime_exit(void)
{
	LOG_INF("Lua runtime exit (Ctrl+F)");
	LOG_DBG("Runtime exit begin running=%d exited=%d has_L=%d", lua_ctx.running,
		lua_ctx.exited, lua_ctx.L != NULL);

	/* Use a fresh exit signal for this cycle. */
	k_sem_reset(&lua_ctx.repl_exited_sem);

	/* Install break hook before flipping runtime flags. */
	if (lua_ctx.L) {
		LOG_DBG("Runtime exit installing break hook");
		lua_ctx.interrupted = true;
		lua_hook_arm();
	}

	/* Set exit flag to stop REPL thread from restarting */
	lua_ctx.exited = true;

	/* Stop current VM */
	lua_ctx.running = false;
	LOG_DBG("Runtime exit flags set running=%d exited=%d", lua_ctx.running, lua_ctx.exited);

	/* Wait for REPL cleanup before proceeding to deep sleep. */
	if (k_current_get() == &lua_ctx.repl_thread) {
		LOG_DBG("Runtime exit called from REPL thread, skipping wait");
	} else {
		int ret = k_sem_take(&lua_ctx.repl_exited_sem, K_MSEC(3000));
		if (ret != 0) {
			LOG_WRN("REPL thread did not exit cleanly within timeout (ret=%d)", ret);
			/* Force-stop REPL if graceful shutdown timed out. */
			k_thread_abort(&lua_ctx.repl_thread);
			k_thread_join(&lua_ctx.repl_thread, K_FOREVER);
			LOG_WRN("REPL thread force-aborted for deep sleep");

			/* REPL cleanup is skipped when aborted, so deinit services here. */
			halo_lua_service_notify(HALO_LUA_EVENT_DEINIT);

			/* Close Lua state if REPL did not reach normal cleanup. */
			k_spinlock_key_t key = k_spin_lock(&lua_ctx.hook_lock);
			lua_State *L = lua_ctx.L;

			lua_ctx.L = NULL;
			k_spin_unlock(&lua_ctx.hook_lock, key);
			if (L) {
				lua_close(L);
			}
			atomic_clear(&lua_ctx.pending);
		} else {
			LOG_DBG("REPL thread exited cleanly");
		}
	}

	LOG_DBG("Runtime exit done");

	lua_ctx.initialized = false;
}
