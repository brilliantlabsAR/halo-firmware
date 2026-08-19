/*
 * Copyright (c) 2026 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "lua.h"
#include "lauxlib.h"

#include <halo/lua_sound.h>
#include <halo/sfxr.h>

LOG_MODULE_REGISTER(lua_sound, CONFIG_HALO_LOG_LEVEL);

static int get_optional_int_field(lua_State *L, int index, const char *field, int fallback)
{
	int value = fallback;

	lua_getfield(L, index, field);
	if (!lua_isnil(L, -1)) {
		value = luaL_checkinteger(L, -1);
	}
	lua_pop(L, 1);

	return value;
}

static const char *sound_error_string(int ret)
{
	switch (ret) {
	case -ENOENT:
		return "unknown sound preset";
	case -EBUSY:
		return "speaker is busy";
	case -ENOMEM:
		return "not enough memory";
	case -EINVAL:
		return "invalid sound arguments";
	case -ENOTSUP:
		return "sound playback is not supported";
	default:
		return "sound playback failed";
	}
}

static void lua_sound_read_args(lua_State *L, int options_index, uint32_t *seed,
				struct halo_sfxr_play_options *opts)
{
	int nargs = lua_gettop(L);
	bool have_seed = false;

	if (nargs >= options_index && !lua_isnil(L, options_index)) {
		luaL_checktype(L, options_index, LUA_TTABLE);

		lua_getfield(L, options_index, "seed");
		if (!lua_isnil(L, -1)) {
			*seed = (uint32_t)luaL_checkinteger(L, -1);
			have_seed = true;
		}
		lua_pop(L, 1);

		opts->sample_rate =
			get_optional_int_field(L, options_index, "sample_rate", opts->sample_rate);
		opts->duration_ms =
			get_optional_int_field(L, options_index, "duration_ms", opts->duration_ms);
		opts->volume = get_optional_int_field(L, options_index, "volume", opts->volume);
	}

	/* The speaker only supports these rates (mirrors frame.speaker.start). */
	if (opts->sample_rate != 8000 && opts->sample_rate != 16000) {
		luaL_error(L, "sample_rate must be 8000 or 16000");
	}

	/* Random by default: a named sound with no explicit seed varies each call.
	 * The cycle counter is a dependency-free entropy source, fine for a
	 * non-cryptographic sound seed. */
	if (!have_seed) {
		*seed = k_cycle_get_32();
	}
}

static int lua_sound_push_result(lua_State *L, const char *name, int ret)
{
	if (ret != 0) {
		LOG_WRN("frame.sound.%s failed: %d", name, ret);
		lua_pushnil(L);
		lua_pushstring(L, sound_error_string(ret));
		lua_pushinteger(L, ret);
		return 3;
	}

	lua_pushboolean(L, true);
	return 1;
}

/**
 * @brief Lua: frame.sound.play(name[, options])
 *
 * Named sounds (pickup, laser, explosion, powerup, hit, jump, blip) are random
 * by default; pass a seed in options for a deterministic, repeatable variant.
 *
 * Examples:
 *   frame.sound.play("laser")                                  -- random each call
 *   frame.sound.play("powerup", {seed=84, duration_ms=1000, volume=20})
 */
static int lua_sound_play(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	uint32_t seed = 1;
	struct halo_sfxr_play_options opts = {
		.sample_rate = 16000,
		.duration_ms = 1000,
		.volume = 20,
		.owner = AUDIO_OWNER_LUA,
	};

	lua_sound_read_args(L, 2, &seed, &opts);

	int ret = halo_sfxr_play_named(name, seed, &opts);

	return lua_sound_push_result(L, name, ret);
}

/**
 * @brief Lua: frame.sound.play_async(name[, options])
 */
static int lua_sound_play_async(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	uint32_t seed = 1;
	struct halo_sfxr_play_options opts = {
		.sample_rate = 16000,
		.duration_ms = 1000,
		.volume = 20,
		.owner = AUDIO_OWNER_LUA,
	};

	lua_sound_read_args(L, 2, &seed, &opts);

	int ret = halo_sfxr_play_named_async(name, seed, &opts);

	return lua_sound_push_result(L, name, ret);
}

/**
 * @brief Lua: frame.sound.stop()
 */
static int lua_sound_stop(lua_State *L)
{
	ARG_UNUSED(L);

	halo_sfxr_stop_async();
	return 0;
}

/**
 * @brief Lua: frame.sound.is_playing()
 */
static int lua_sound_is_playing(lua_State *L)
{
	lua_pushboolean(L, halo_sfxr_is_async_playing());
	return 1;
}

int lua_open_sound_library(lua_State *L)
{
	lua_getglobal(L, "frame");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setglobal(L, "frame");
	}

	lua_newtable(L);

	static const luaL_Reg sound_funcs[] = {
		{"play", lua_sound_play},
		{"play_async", lua_sound_play_async},
		{"stop", lua_sound_stop},
		{"is_playing", lua_sound_is_playing},
		{NULL, NULL},
	};

	luaL_setfuncs(L, sound_funcs, 0);
	lua_setfield(L, -2, "sound");
	lua_pop(L, 1);

	LOG_DBG("Sound library registered successfully");

	return 0;
}
