/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_LUA_LOG_H_
#define HALO_LUA_LOG_H_

#include <lua.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open the log library for Lua.
 *
 * Registers frame.log namespace with:
 *   frame.log.list()  — list log files
 *   frame.log.read(id)— read a log file by index
 *   frame.log.clear() — delete all log files
 *
 * @param L Lua state
 * @return 0
 */
int lua_open_log_library(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif /* HALO_LUA_LOG_H_ */
