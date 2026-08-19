/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_LUA_COMPRESSION_H
#define HALO_LUA_COMPRESSION_H

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file lua_compression.h
 * @brief Lua compression library for Halo platform
 * 
 * Provides LZ4 decompression functionality to Lua scripts.
 * Useful for decompressing data received over Bluetooth or stored in files.
 * 
 * The decompression process works in blocks:
 * 1. Call decompress() with compressed data and block size
 * 2. Library decompresses data block by block
 * 3. Each decompressed block triggers the process_function callback
 * 
 * All functions are registered under frame.compression table.
 */

/**
 * @brief Open and register the compression library with Lua VM
 * 
 * This function registers all compression functions under frame.compression table.
 * It should be called during Lua VM initialization.
 * 
 * Example Lua usage:
 * @code{.lua}
 * -- Register callback to process decompressed data
 * frame.compression.process_function(function(data)
 *     print("Decompressed block: " .. #data .. " bytes")
 *     -- Process the decompressed data
 *     frame.file.open("output.bin", "a"):write(data):close()
 * end)
 * 
 * -- Decompress data (received via Bluetooth or from file)
 * frame.compression.decompress(compressed_data, 4096)  -- 4KB blocks
 * 
 * -- Clear callback when done
 * frame.compression.process_function(nil)
 * @endcode
 * 
 * @param L Lua state
 * @return 0 on success
 */
int lua_open_compression_library(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif /* HALO_LUA_COMPRESSION_H */
