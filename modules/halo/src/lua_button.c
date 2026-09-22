/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/input/button.h>
#include <zephyr/drivers/sm/sm.h>
#include <zephyr/sys/reboot.h>

#include "lua.h"
#include "lauxlib.h"

#include <halo/lua_button.h>
#include <halo/lua_service.h>
#include <halo/lua_runtime.h>
#include <halo/pm_manager.h>
#include <halo/led_manager.h>
#include <halo/ble_connection.h>
#include <halo/ble_security.h>
#include <halo/battery_manager.h>
#include <halo/file_manager.h>
#include <halo/sfxr.h>
#include <halo/audio_stream.h>

LOG_MODULE_REGISTER(lua_button, CONFIG_HALO_LOG_LEVEL);

/**
 * @brief Button callback state
 *
 * Stores Lua callback references for different button actions.
 */
static struct {
	int single_click_ref;       /* LUA_REGISTRYINDEX reference for single click */
	int double_click_ref;       /* LUA_REGISTRYINDEX reference for double click */
	int long_press_ref;         /* LUA_REGISTRYINDEX reference for long press (1s) */
	int long_press_level1_ref;  /* LUA_REGISTRYINDEX reference for long press level 1 (2s, deep sleep) */
	int long_press_level2_ref;  /* LUA_REGISTRYINDEX reference for long press level 2 (5s, pairing) */
	int long_press_level3_ref;  /* LUA_REGISTRYINDEX reference for long press level 3 (15s, ship mode) */
} button_callback_state = {
	.single_click_ref = LUA_NOREF,
	.double_click_ref = LUA_NOREF,
	.long_press_ref = LUA_NOREF,
	.long_press_level1_ref = LUA_NOREF,
	.long_press_level2_ref = LUA_NOREF,
	.long_press_level3_ref = LUA_NOREF,
};

/* Pending button events, produced on the button driver thread and drained
 * on the Lua thread by the runtime's shared hook. Single producer / single
 * consumer; monotonically increasing indices. Entries hold the callback
 * registry reference captured at event time. */
#define BUTTON_RING_SIZE 8 /* power of two */
static struct {
	struct {
		int ref;
		const char *action_name; /* for logging */
	} ev[BUTTON_RING_SIZE];
	atomic_t head; /* next write slot */
	atomic_t tail; /* next read slot */
} button_ring;

/**
 * @brief Drain pending button events into their Lua callbacks (REPL thread)
 */
static void button_drain(lua_State *L)
{
	while (atomic_get(&button_ring.tail) != atomic_get(&button_ring.head)) {
		unsigned int slot = atomic_get(&button_ring.tail) & (BUTTON_RING_SIZE - 1);
		int ref = button_ring.ev[slot].ref;
		const char *action_name = button_ring.ev[slot].action_name;

		atomic_inc(&button_ring.tail);

		if (ref == LUA_NOREF) {
			continue;
		}

		lua_rawgeti(L, LUA_REGISTRYINDEX, ref);

		if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
			const char *error = lua_tostring(L, -1);
			LOG_ERR("Button %s callback error: %s", action_name, error);
			lua_pop(L, 1);
		}
	}
}

/**
 * @brief Queue a Lua button callback for the Lua thread
 *
 * @param ref Callback reference in LUA_REGISTRYINDEX
 * @param action_name Action name for logging (e.g., "single click")
 */
static void run_lua_button_callback(int ref, const char *action_name)
{
	if (halo_pm_is_sleeping()) {
		halo_pm_wakeup(HALO_PM_WAKEUP_BUTTON);
	}

	if (ref == LUA_NOREF) {
		return;
	}

	if (!halo_lua_is_running()) {
		return;
	}

	if (atomic_get(&button_ring.head) - atomic_get(&button_ring.tail) >= BUTTON_RING_SIZE) {
		LOG_WRN("Button event queue full, dropping %s", action_name);
		return;
	}

	unsigned int slot = atomic_get(&button_ring.head) & (BUTTON_RING_SIZE - 1);

	button_ring.ev[slot].ref = ref;
	button_ring.ev[slot].action_name = action_name;
	atomic_inc(&button_ring.head);

	halo_lua_event_signal(HALO_LUA_EVENT_SRC_BUTTON);
}

/**
 * @brief Zephyr button event callback for single click
 */
static void button_event_cb_single_click(const struct device *dev, enum button_action action)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(action);
	run_lua_button_callback(button_callback_state.single_click_ref, "single click");
}

/**
 * @brief Zephyr button event callback for double click
 */
static void button_event_cb_double_click(const struct device *dev, enum button_action action)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(action);

	run_lua_button_callback(button_callback_state.double_click_ref, "double click");
}

/**
 * @brief Zephyr button event callback for long press
 */
static void button_event_cb_long_press(const struct device *dev, enum button_action action)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(action);

	run_lua_button_callback(button_callback_state.long_press_ref, "long press");
}

#ifdef CONFIG_HALO_POWER_CUE_SOUND
/**
 * @brief Crossing cue: the hold just reached the power-off threshold.
 *
 * Runs in the button driver's thread while the button is STILL HELD, so it
 * must not block — the same thread detects the release that actually powers
 * off. The async player streams the blip on its own thread; if the user
 * releases mid-blip, the deep-sleep quiesce stops it and the shutdown
 * sound's speaker-free retry loop covers any remaining teardown.
 */
static void button_event_cb_power_cue(const struct device *dev, enum button_action action)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(action);

	const struct halo_sfxr_play_options opts = {
		.duration_ms = CONFIG_HALO_POWER_CUE_SOUND_DURATION_MS,
		.volume = CONFIG_HALO_POWER_CUE_SOUND_VOLUME,
		.owner = AUDIO_OWNER_SYSTEM,
	};

	int ret = halo_sfxr_play_named_async(CONFIG_HALO_POWER_CUE_SOUND_PRESET,
					     CONFIG_HALO_POWER_CUE_SOUND_SEED, &opts);
	if (ret != 0) {
		LOG_DBG("Power cue not played: %d", ret);
	}
}
#endif

static void button_event_cb_long_press_level1(const struct device *dev, enum button_action action)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(action);

	LOG_INF("2 second hold triggered - entering deep sleep");

	int ret = halo_pm_sleep_deep(0);

	/* On success the device powers off (or reboots if entry was blocked)
	 * and this never returns. Reaching here means the request was refused
	 * before anything was torn down; the device is still fully running. */
	LOG_ERR("Deep sleep request refused: %d", ret);
}

static void button_event_cb_long_press_level2(const struct device *dev, enum button_action action)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(action);

	LOG_INF("5 second hold triggered - opening pairing window");

#ifdef CONFIG_HALO_PAIRING_CUE_SOUND
	/* Pairing cue: a coin "buh-ding" as the window opens. Async for the
	 * same reason as the power cue - this runs on the button driver's
	 * thread. Nothing races it: no shutdown follows pairing. */
	{
		const struct halo_sfxr_play_options cue_opts = {
			.duration_ms = CONFIG_HALO_PAIRING_CUE_SOUND_DURATION_MS,
			.volume = CONFIG_HALO_PAIRING_CUE_SOUND_VOLUME,
			.owner = AUDIO_OWNER_SYSTEM,
		};

		(void)halo_sfxr_play_named_async(CONFIG_HALO_PAIRING_CUE_SOUND_PRESET,
						 CONFIG_HALO_PAIRING_CUE_SOUND_SEED,
						 &cue_opts);
	}
#endif

	/* Free the (single) connection so the new device can get in; existing
	 * bonds are kept - the window only admits a new peer (see PAIRING.md). */
	halo_ble_conn_disconnect();

	int ret = halo_ble_sec_pairing_window_open();
	if (ret != 0) {
		LOG_ERR("Failed to open pairing window: %d", ret);
	}
}

static void button_event_cb_long_press_level3(const struct device *dev, enum button_action action)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(action);

	LOG_INF("15 second hold triggered - level 3");

	if (halo_battery_is_charging()) {
		LOG_WRN("Cannot enter ship mode while charging");
		return;
	}

	LOG_WRN("Entering ship mode - device will shut down completely");
	LOG_WRN("Hardware reset required to wake up!");

	/* Stop any LED activity */
	halo_led_clear_state(HALO_LED_PRIORITY_HIGH);

	/* Factory reset: wipe the entire filesystem */
	LOG_WRN("Performing factory reset (formatting filesystem)...");
	halo_file_format();

	/* Get the shutdown device */
	const struct device *sm = DEVICE_DT_GET(DT_ALIAS(shutdown));
	if (!device_is_ready(sm)) {
		LOG_ERR("Ship mode device not ready");
		return;
	}

	/* Give time for log messages to be sent */
	k_sleep(K_MSEC(100));

	/* Enter ship mode (shutdown) */
	shutdown(sm);
}

/**
 * @brief Lua: frame.button.single(function)
 *
 * Register or clear single click callback.
 *
 * @param L Lua state
 * @return 0
 */
static int lua_button_single(lua_State *L)
{
	/* Check if clearing callback (nil argument) */
	if (lua_isnil(L, 1)) {
		if (button_callback_state.single_click_ref != LUA_NOREF) {
			luaL_unref(L, LUA_REGISTRYINDEX, button_callback_state.single_click_ref);
			button_callback_state.single_click_ref = LUA_NOREF;
		}
		LOG_DBG("Button single click callback cleared");
		return 0;
	}

	/* Must be a function */
	if (!lua_isfunction(L, 1)) {
		return luaL_error(L, "expected function or nil");
	}

	/* Unref old callback if exists */
	if (button_callback_state.single_click_ref != LUA_NOREF) {
		luaL_unref(L, LUA_REGISTRYINDEX, button_callback_state.single_click_ref);
	}

	/* Store new callback in registry */
	/* luaL_ref takes the stack top: drop any extra arguments so the
	 * function in slot 1 is what gets registered. */
	lua_settop(L, 1);
	button_callback_state.single_click_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	LOG_DBG("Button single click callback registered");
	return 0;
}

/**
 * @brief Lua: frame.button.double(function)
 *
 * Register or clear double click callback.
 *
 * @param L Lua state
 * @return 0
 */
static int lua_button_double(lua_State *L)
{
	/* Check if clearing callback (nil argument) */
	if (lua_isnil(L, 1)) {
		if (button_callback_state.double_click_ref != LUA_NOREF) {
			luaL_unref(L, LUA_REGISTRYINDEX, button_callback_state.double_click_ref);
			button_callback_state.double_click_ref = LUA_NOREF;
		}
		LOG_DBG("Button double click callback cleared");
		return 0;
	}

	/* Must be a function */
	if (!lua_isfunction(L, 1)) {
		return luaL_error(L, "expected function or nil");
	}

	/* Unref old callback if exists */
	if (button_callback_state.double_click_ref != LUA_NOREF) {
		luaL_unref(L, LUA_REGISTRYINDEX, button_callback_state.double_click_ref);
	}

	/* Store new callback in registry */
	/* luaL_ref takes the stack top: drop any extra arguments so the
	 * function in slot 1 is what gets registered. */
	lua_settop(L, 1);
	button_callback_state.double_click_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	LOG_DBG("Button double click callback registered");
	return 0;
}

/**
 * @brief Lua: frame.button.long(function)
 *
 * Register or clear long press callback.
 *
 * @param L Lua state
 * @return 0
 */
static int lua_button_long(lua_State *L)
{
	/* Check if clearing callback (nil argument) */
	if (lua_isnil(L, 1)) {
		if (button_callback_state.long_press_ref != LUA_NOREF) {
			luaL_unref(L, LUA_REGISTRYINDEX, button_callback_state.long_press_ref);
			button_callback_state.long_press_ref = LUA_NOREF;
		}
		LOG_DBG("Button long press callback cleared");
		return 0;
	}

	/* Must be a function */
	if (!lua_isfunction(L, 1)) {
		return luaL_error(L, "expected function or nil");
	}

	/* Unref old callback if exists */
	if (button_callback_state.long_press_ref != LUA_NOREF) {
		luaL_unref(L, LUA_REGISTRYINDEX, button_callback_state.long_press_ref);
	}

	/* Store new callback in registry */
	/* luaL_ref takes the stack top: drop any extra arguments so the
	 * function in slot 1 is what gets registered. */
	lua_settop(L, 1);
	button_callback_state.long_press_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	LOG_DBG("Button long press callback registered");
	return 0;
}

/* ============================================================================
 * Power Management Integration
 * ============================================================================ */

/**
 * @brief Service lifecycle event handler
 *
 * Handles lifecycle events like INIT, DEINIT, INTERRUPT, RESTART.
 */
static int button_service_event_handler(halo_lua_event_t event, void *user_data)
{
	ARG_UNUSED(user_data);

	int ret = 0;

	switch (event) {
	case HALO_LUA_EVENT_INIT:
		button_callback_state.single_click_ref = LUA_NOREF;
		button_callback_state.double_click_ref = LUA_NOREF;
		button_callback_state.long_press_ref = LUA_NOREF;
		button_callback_state.long_press_level1_ref = LUA_NOREF;
		button_callback_state.long_press_level2_ref = LUA_NOREF;
		button_callback_state.long_press_level3_ref = LUA_NOREF;
		atomic_set(&button_ring.tail, atomic_get(&button_ring.head));
		halo_lua_event_drain_set(HALO_LUA_EVENT_SRC_BUTTON, button_drain);
		break;

	case HALO_LUA_EVENT_DEINIT:
		/* Callbacks will be cleaned up by Lua GC */
		button_callback_state.single_click_ref = LUA_NOREF;
		button_callback_state.double_click_ref = LUA_NOREF;
		button_callback_state.long_press_ref = LUA_NOREF;
		button_callback_state.long_press_level1_ref = LUA_NOREF;
		button_callback_state.long_press_level2_ref = LUA_NOREF;
		button_callback_state.long_press_level3_ref = LUA_NOREF;
		atomic_set(&button_ring.tail, atomic_get(&button_ring.head));
		break;

	default:
		break;
	}

	return ret;
}

/* Register service with lifecycle management (always-on service) */
HALO_LUA_SERVICE_DEFINE(button_service, button_service_event_handler, NULL, true);

/**
 * @brief Open and register button library with Lua VM
 *
 * Registers all button functions under frame.button table and
 * initializes the button hardware.
 */
int lua_open_button_library(lua_State *L)
{
	/* Register service for lifecycle management */
	int ret = halo_lua_service_register(&button_service);
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("Failed to register button service: %d", ret);
		return ret;
	}

	/* Initialize the button device */
	const struct device *button = DEVICE_DT_GET(DT_ALIAS(sw0));
	if (!device_is_ready(button)) {
		LOG_ERR("Button device not ready!");
		return -ENODEV;
	}

	/* Register button event callbacks with Zephyr driver */
	button_event_callback_register(button, button_event_cb_single_click, BUTTON_SINGLE_CLICK);
	button_event_callback_register(button, button_event_cb_double_click, BUTTON_DOUBLE_CLICK);
	button_event_callback_register(button, button_event_cb_long_press, BUTTON_LONG_PRESS);
	button_event_callback_register(button, button_event_cb_long_press_level1,
				       BUTTON_LONG_PRESS_LEVEL_1);
	button_event_callback_register(button, button_event_cb_long_press_level2,
				       BUTTON_LONG_PRESS_LEVEL_2);
	button_event_callback_register(button, button_event_cb_long_press_level3,
				       BUTTON_LONG_PRESS_LEVEL_3);
#ifdef CONFIG_HALO_POWER_CUE_SOUND
	button_event_callback_register(button, button_event_cb_power_cue,
				       BUTTON_LONG_PRESS_LEVEL_1_HELD);
#endif

	/* Get or create the global 'frame' table */
	lua_getglobal(L, "frame");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setglobal(L, "frame");
	}

	/* Create 'button' subtable */
	lua_newtable(L);

	/* Register button functions */
	lua_pushcfunction(L, lua_button_single);
	lua_setfield(L, -2, "single");

	lua_pushcfunction(L, lua_button_double);
	lua_setfield(L, -2, "double");

	lua_pushcfunction(L, lua_button_long);
	lua_setfield(L, -2, "long");

	/* Set 'button' table in 'frame' */
	lua_setfield(L, -2, "button");

	/* Pop frame table */
	lua_pop(L, 1);

	return 0;
}
