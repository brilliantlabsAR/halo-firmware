/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_LUA_IMU_H
#define HALO_LUA_IMU_H

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file lua_imu.h
 * @brief Lua IMU (Inertial Measurement Unit) library for Halo platform
 * 
 * Provides IMU sensor functionality to Lua scripts including:
 * - Accelerometer data (raw and processed)
 * - Magnetometer/Compass data
 * - Direction/orientation (pitch, roll, heading)
 * - Tap/motion event detection with callbacks
 * 
 * All functions are registered under frame.imu table.
 */

/**
 * @brief Open and register the IMU library with Lua VM
 * 
 * This function registers all IMU functions under frame.imu table and
 * initializes the IMU hardware (accelerometer and magnetometer).
 * It should be called during Lua VM initialization.
 * 
 * Example Lua usage:
 * @code{.lua}
 * -- Configure IMU sensors
 * frame.imu.config({
 *     accelerometer = {
 *         sampling_frequency = 200,  -- 200 Hz
 *         full_scale = 16            -- ±16g
 *     },
 *     magnetometer = {
 *         sampling_frequency = 100,  -- 100 Hz
 *         full_scale = 4             -- ±4 Gauss
 *     }
 * })
 * 
 * -- Get device orientation
 * local dir = frame.imu.direction()
 * print("Pitch: " .. dir.pitch)
 * print("Roll: " .. dir.roll)
 * print("Heading: " .. dir.heading)
 * 
 * -- Get raw sensor data
 * local raw = frame.imu.raw()
 * print("Accel X: " .. raw.accelerometer.x .. " mg")
 * print("Accel Y: " .. raw.accelerometer.y .. " mg")
 * print("Accel Z: " .. raw.accelerometer.z .. " mg")
 * print("Compass X: " .. raw.compass.x .. " mG")
 * print("Compass Y: " .. raw.compass.y .. " mG")
 * print("Compass Z: " .. raw.compass.z .. " mG")
 * 
 * -- Register tap detection callback
 * frame.imu.tap_callback(function()
 *     print("Tap detected!")
 * end)
 * 
 * -- Clear tap callback
 * frame.imu.tap_callback(nil)
 * @endcode
 * 
 * @param L Lua state
 * @return 0 on success, negative error code on failure
 */
int lua_open_imu_library(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif /* HALO_LUA_IMU_H */
