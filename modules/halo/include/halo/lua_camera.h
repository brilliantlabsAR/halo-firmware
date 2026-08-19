/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_LUA_CAMERA_H_
#define HALO_LUA_CAMERA_H_

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open and register camera library with Lua VM
 * 
 * Registers all camera functions under frame.camera table and
 * initializes the camera hardware with libmpix image processing.
 * 
 * API compatibility: frame.camera.*
 * - frame.camera.capture(options)
 * - frame.camera.image_ready()
 * - frame.camera.read(bytes)
 * - frame.camera.read_raw(bytes)
 * - frame.camera.power_save(enable)
 * 
 * @param L Lua state
 * @return 0 on success, negative errno otherwise
 */
int lua_open_camera_library(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif /* HALO_LUA_CAMERA_H_ */
