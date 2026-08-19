/*
 * Copyright (c) 2026 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_LUA_SOUND_H_
#define HALO_LUA_SOUND_H_

#include "lua.h"

/**
 * @brief Open and register sound library with Lua VM
 *
 * Registers frame.sound table with:
 * - play(name[, seed[, options]])
 * - play_async(name[, seed[, options]])
 * - stop()
 * - is_playing()
 *
 * @param L Lua state
 * @return 0 on success
 */
int lua_open_sound_library(lua_State *L);

#endif /* HALO_LUA_SOUND_H_ */
