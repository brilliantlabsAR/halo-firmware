/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_LUA_MICROPHONE_H_
#define HALO_LUA_MICROPHONE_H_

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register microphone library to Lua
 *
 * Creates frame.microphone table with functions:
 *   - start(config) - Start microphone capture
 *   - stop() - Stop microphone capture
 *   - read(bytes) - Non-blocking read from ring buffer (PCM or LC3; partial OK)
 *   - aad_callback(func) - Set acoustic activity detection callback
 *
 * @param L Lua state
 * @return 0 on success
 */
int lua_open_microphone_library(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif /* HALO_LUA_MICROPHONE_H_ */
