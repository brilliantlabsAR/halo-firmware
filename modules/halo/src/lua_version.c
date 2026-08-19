/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <app_version.h>
#include <se_service.h>

#include "lua.h"
#include "lauxlib.h"

LOG_MODULE_REGISTER(lua_version, CONFIG_HALO_LOG_LEVEL);

/**
 * @brief Load version information into Lua 'frame' table
 * 
 * Populates the following fields:
 *   frame.HARDWARE_VERSION - Board name (e.g., "halo")
 *   frame.FIRMWARE_VERSION - App version string (e.g., "1.0.0")
 *   frame.GIT_TAG          - Git build version
 *   frame.SE_REVISION      - Secure Enclave firmware revision (lazy loaded via frame.get_se_revision())
 */
void halo_lua_load_version_info(lua_State *L)
{
	lua_getglobal(L, "frame");

	/* Hardware version (board name) */
	lua_pushstring(L, CONFIG_BOARD);
	lua_setfield(L, -2, "HARDWARE_VERSION");

	/* Firmware version */
	lua_pushstring(L, APP_VERSION_STRING);
	lua_setfield(L, -2, "FIRMWARE_VERSION");

	/* Git tag */
	lua_pushstring(L, STRINGIFY(APP_BUILD_VERSION));
	lua_setfield(L, -2, "GIT_TAG");

	/* SE revision is now lazy loaded via frame.get_se_revision() */

	lua_pop(L, 1);
}
