/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_LUA_FILE_H
#define HALO_LUA_FILE_H

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file lua_file.h
 * @brief Lua file system library for Halo platform
 * 
 * Provides file system operations to Lua scripts including:
 * - File I/O (open, read, write, close)
 * - Directory operations (list, create, remove)
 * - File operations (rename, remove)
 * - Module loading (custom require)
 * 
 * All functions are registered under frame.file table.
 */

/**
 * @brief Open and register the file library with Lua VM
 * 
 * This function registers all file functions under frame.file table.
 * It also overrides the global 'require' function to load from /lfs/.
 * It should be called during Lua VM initialization.
 * 
 * Example Lua usage:
 * @code{.lua}
 * -- Open file for reading
 * local file = frame.file.open("test.txt", "r")
 * if file then
 *     local line = file:read()
 *     print(line)
 *     file:close()
 * end
 * 
 * -- Write to file
 * local file = frame.file.open("output.txt", "w")
 * file:write("Hello World\n")
 * file:close()
 * 
 * -- Append to file
 * local file = frame.file.open("log.txt", "a")
 * file:write("Log entry\n")
 * file:close()
 * 
 * -- List directory
 * local entries = frame.file.listdir("/")
 * for i, entry in ipairs(entries) do
 *     print(entry.name, entry.type, entry.size)
 * end
 * 
 * -- Create directory
 * frame.file.mkdir("mydir")
 * frame.file.mkdir("mydir/subdir")  -- Creates recursively
 * 
 * -- Rename file/directory
 * frame.file.rename("old.txt", "new.txt")
 * 
 * -- Remove file/directory
 * frame.file.remove("file.txt")
 * 
 * -- Load Lua module from /lfs/
 * require("mymodule")  -- Loads /lfs/mymodule.lua
 * @endcode
 * 
 * @param L Lua state
 * @return 0 on success
 */
int lua_open_file_library(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif /* HALO_LUA_FILE_H */
