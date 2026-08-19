/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_LUA_ANCS_H_
#define HALO_LUA_ANCS_H_

#include "lua.h"

/**
 * @brief Open and register the ANCS library with the Lua VM
 *
 * Registers the frame.ancs table (Apple Notification Center Service
 * client API) and hooks the BLE ANCS client callbacks.
 *
 * @param L Lua state
 * @return 0 on success, negative errno on error
 */
int lua_open_ancs_library(lua_State *L);

#endif /* HALO_LUA_ANCS_H_ */
