/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_LUA_VERSION_H_
#define HALO_LUA_VERSION_H_

#include "lua.h"

/**
 * @brief Load version information into Lua 'frame' table
 * 
 * Populates version-related fields in the global 'frame' table:
 * - frame.HARDWARE_VERSION: Board name
 * - frame.FIRMWARE_VERSION: Application version
 * - frame.GIT_TAG: Build tag
 * - frame.SE_REVISION: (deprecated, use frame.get_se_revision() instead)
 * 
 * Note: SE_REVISION is no longer populated at initialization time to avoid
 * performance overhead. Use frame.get_se_revision() to retrieve it on demand.
 * 
 * @param L Lua state
 */
void halo_lua_load_version_info(lua_State *L);

#endif /* HALO_LUA_VERSION_H_ */
