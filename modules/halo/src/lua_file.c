/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include "lua.h"
#include "lauxlib.h"
#include <halo/lua_file.h>
#include <halo/lua_service.h>
#include <halo/file_manager.h>
#include <halo/mem_manager.h>

LOG_MODULE_REGISTER(lua_file, CONFIG_HALO_LOG_LEVEL);

/* File stream userdata structure */
typedef struct {
	struct fs_file_t file;
	bool is_open;
} file_stream_t;

/**
 * @brief Check if file stream is open, error if closed
 */
static void check_file_open(lua_State *L, file_stream_t *stream)
{
	if (!stream->is_open) {
		luaL_error(L, "attempt to use a closed file");
	}
}

/**
 * @brief Get full path with /lfs prefix
 *
 * @param filename Relative filename
 * @param buffer Output buffer
 * @param size Buffer size
 * @return 0 on success, negative on error
 */
static int get_full_path(const char *filename, char *buffer, size_t size)
{
	const char *mount_point = halo_file_mount_point();
	int ret = snprintf(buffer, size, "%s/%s", mount_point, filename);

	if (ret < 0 || ret >= size) {
		return -ENOMEM;
	}

	return 0;
}

/**
 * @brief Lua: file:read()
 *
 * Read a line from file (up to newline or EOF).
 * Returns string on success, nil on EOF/error.
 */
static int lua_file_read(lua_State *L)
{
	file_stream_t *stream = (file_stream_t *)luaL_checkudata(L, 1, LUA_FILEHANDLE);
	check_file_open(L, stream);

	/* Binary mode: file:read(n) reads exactly up to n raw bytes (no newline
	 * handling), so binary payloads (e.g. raw PCM, which contains 0x0A bytes)
	 * round-trip intact. Returns the bytes read, or nil at EOF. */
	if (lua_isnumber(L, 2)) {
		lua_Integer want = lua_tointeger(L, 2);
		if (want <= 0) {
			lua_pushstring(L, "");
			return 1;
		}
		if (want > LUAL_BUFFERSIZE) {
			want = LUAL_BUFFERSIZE;
		}
		luaL_Buffer bbuf;
		char *out = luaL_buffinitsize(L, &bbuf, (size_t)want);
		ssize_t got = fs_read(&stream->file, out, (size_t)want);
		if (got < 0) {
			LOG_ERR("File read error: %d", (int)got);
			return luaL_error(L, "error reading file: %d", (int)got);
		}
		if (got == 0) {
			lua_pushnil(L); /* EOF */
			return 1;
		}
		luaL_pushresultsize(&bbuf, (size_t)got);
		return 1;
	}

	luaL_Buffer buffer;
	luaL_buffinit(L, &buffer);

	char ch;
	size_t count = 0;

	while (count < LUAL_BUFFERSIZE) {
		ssize_t result = fs_read(&stream->file, &ch, 1);

		if (result < 0) {
			LOG_ERR("File read error: %d", (int)result);
			return luaL_error(L, "error reading file: %d", (int)result);
		}

		if (result == 0) {
			/* EOF */
			if (count == 0) {
				lua_pushnil(L);
				return 1;
			}
			break;
		}

		if (ch == '\n') {
			/* End of line - don't include newline */
			if (count == 0) {
				lua_pushstring(L, "");
				return 1;
			}
			break;
		}

		luaL_addchar(&buffer, ch);
		count++;
	}

	luaL_pushresult(&buffer);
	return 1;
}

/**
 * @brief Lua: file:write(string)
 *
 * Write string to file.
 */
static int lua_file_write(lua_State *L)
{
	file_stream_t *stream = (file_stream_t *)luaL_checkudata(L, 1, LUA_FILEHANDLE);
	check_file_open(L, stream);

	/* Check if file was opened for writing */
	if ((stream->file.flags & FS_O_RDWR) == FS_O_READ) {
		return luaL_error(L, "file opened in read-only mode");
	}

	size_t length;
	const char *data = luaL_checklstring(L, 2, &length);

	ssize_t result = fs_write(&stream->file, data, length);

	if (result < 0) {
		LOG_ERR("File write error: %d", (int)result);
		return luaL_error(L, "error writing to file: %d", (int)result);
	}

	if (result != length) {
		LOG_ERR("Partial write: %d of %u bytes", (int)result, length);
		return luaL_error(L, "incomplete write: %d of %d bytes", (int)result, (int)length);
	}

	return 0;
}

/**
 * @brief Lua: file:close()
 *
 * Close file handle.
 */
static int lua_file_close(lua_State *L)
{
	file_stream_t *stream = (file_stream_t *)luaL_checkudata(L, 1, LUA_FILEHANDLE);

	if (!stream->is_open) {
		return 0; /* Already closed */
	}

	int ret = fs_close(&stream->file);
	if (ret < 0) {
		LOG_WRN("File close error: %d", ret);
	}

	stream->is_open = false;
	LOG_DBG("File closed");
	return 0;
}

/**
 * @brief Lua: frame.file.open(filename, mode)
 *
 * Open file for reading/writing.
 * Modes: "r" (read), "w" (write/truncate), "a" (append)
 *
 * Returns file handle. Throws error on failure.
 */
static int lua_file_open(lua_State *L)
{
	const char *filename = luaL_checkstring(L, 1);
	const char *mode = luaL_optstring(L, 2, "r");

	LOG_DBG("File open: '%s' mode '%s'", filename, mode);

	/* Create userdata for file stream */
	file_stream_t *stream = (file_stream_t *)lua_newuserdatauv(L, sizeof(file_stream_t), 0);
	stream->is_open = false;
	luaL_setmetatable(L, LUA_FILEHANDLE);

	/* Parse mode */
	int mode_flags = 0;
	switch (mode[0]) {
	case 'r':
		mode_flags = FS_O_READ;
		break;
	case 'w':
		mode_flags = FS_O_RDWR | FS_O_CREATE;
		break;
	case 'a':
		mode_flags = FS_O_RDWR | FS_O_APPEND | FS_O_CREATE;
		break;
	default:
		return luaL_error(L, "mode must be 'r', 'w', or 'a'");
	}

	/* Get full path */
	char full_path[256];
	int ret = get_full_path(filename, full_path, sizeof(full_path));
	if (ret < 0) {
		return luaL_error(L, "filename too long");
	}

	/* Initialize and open file */
	fs_file_t_init(&stream->file);
	ret = fs_open(&stream->file, full_path, mode_flags);

	if (ret < 0) {
		LOG_ERR("Failed to open file '%s': %d", full_path, ret);
		return luaL_error(L, "cannot open file '%s': %d", filename, ret);
	}

	/* If write mode, truncate file */
	if (mode[0] == 'w') {
		ret = fs_truncate(&stream->file, 0);
		if (ret < 0) {
			LOG_WRN("Failed to truncate file: %d", ret);
		}
	}

	stream->is_open = true;
	return 1;
}

/**
 * @brief Lua: frame.file.remove(filename)
 *
 * Delete file or empty directory.
 */
static int lua_file_remove(lua_State *L)
{
	const char *filename = luaL_checkstring(L, 1);

	char full_path[256];
	int ret = get_full_path(filename, full_path, sizeof(full_path));
	if (ret < 0) {
		return luaL_error(L, "filename too long");
	}

	ret = fs_unlink(full_path);
	if (ret < 0) {
		return luaL_error(L, "error removing file/directory: %d", ret);
	}

	return 0;
}

/**
 * @brief Lua: frame.file.remove_all()
 *
 * Remove all files and folders (except settings).
 * Settings file contains pairing information and configuration.
 */
static int lua_file_remove_all(lua_State *L)
{
	int ret = halo_file_remove_all();
	if (ret < 0) {
		return luaL_error(L, "error removing all files: %d", ret);
	}

	return 0;
}

/**
 * @brief Lua: frame.file.rename(from, to)
 *
 * Rename file or directory.
 */
static int lua_file_rename(lua_State *L)
{
	const char *from_name = luaL_checkstring(L, 1);
	const char *to_name = luaL_checkstring(L, 2);

	char from_path[256];
	char to_path[256];

	int ret = get_full_path(from_name, from_path, sizeof(from_path));
	if (ret < 0) {
		return luaL_error(L, "source filename too long");
	}

	ret = get_full_path(to_name, to_path, sizeof(to_path));
	if (ret < 0) {
		return luaL_error(L, "destination filename too long");
	}

	ret = fs_rename(from_path, to_path);
	if (ret < 0) {
		return luaL_error(L, "error renaming file/directory: %d", ret);
	}

	return 0;
}

/**
 * @brief Lua: frame.file.listdir(path)
 *
 * List directory contents.
 * Returns array of tables: {name, size, type}
 * type: 1=file, 2=directory
 */
static int lua_file_listdir(lua_State *L)
{
	const char *dir_name = luaL_checkstring(L, 1);

	char full_path[256];
	int ret = get_full_path(dir_name, full_path, sizeof(full_path));
	if (ret < 0) {
		return luaL_error(L, "path too long");
	}

	struct fs_dir_t dir;
	fs_dir_t_init(&dir);

	ret = fs_opendir(&dir, full_path);
	if (ret < 0) {
		return luaL_error(L, "directory not found: %d", ret);
	}

	/* Create result table */
	lua_newtable(L);
	int index = 1;

	struct fs_dirent entry;
	while (true) {
		ret = fs_readdir(&dir, &entry);
		if (ret < 0) {
			fs_closedir(&dir);
			return luaL_error(L, "error reading directory: %d", ret);
		}

		/* End of directory */
		if (entry.name[0] == '\0') {
			break;
		}

		/* Create entry table */
		lua_newtable(L);

		lua_pushstring(L, entry.name);
		lua_setfield(L, -2, "name");

		lua_pushinteger(L, entry.size);
		lua_setfield(L, -2, "size");

		/* Type: FS_DIR_ENTRY_FILE=0, FS_DIR_ENTRY_DIR=1
		 * Convert to: 1=file, 2=directory */
		lua_pushinteger(L, entry.type + 1);
		lua_setfield(L, -2, "type");

		/* Add to result array */
		lua_seti(L, -2, index++);
	}

	fs_closedir(&dir);
	return 1;
}

/**
 * @brief Lua: frame.file.mkdir(path)
 *
 * Create directory (recursively creates parent directories).
 */
static int lua_file_mkdir(lua_State *L)
{
	const char *dir_name = luaL_checkstring(L, 1);

	/* Create directories recursively */
	char current_path[256];
	const char *p = dir_name;
	const char *next_slash;

	while (true) {
		/* Find next slash or end of string */
		next_slash = strchr(p, '/');

		size_t len;
		if (next_slash) {
			len = next_slash - dir_name;
			p = next_slash + 1;
		} else {
			len = strlen(dir_name);
		}

		/* Build current path */
		char partial[256];
		snprintf(partial, sizeof(partial), "%.*s", (int)len, dir_name);

		int ret = get_full_path(partial, current_path, sizeof(current_path));
		if (ret < 0) {
			return luaL_error(L, "path too long");
		}

		/* Check if directory already exists to avoid EEXIST log from Zephyr */
		struct fs_dirent entry;
		ret = fs_stat(current_path, &entry);
		if (ret == 0) {
			/* Directory/file already exists, skip creation */
			if (next_slash == NULL) {
				break;
			}
			continue;
		}

		/* Create this level */
		ret = fs_mkdir(current_path);
		if (ret < 0) {
			LOG_ERR("Failed to create directory '%s': %d", current_path, ret);
			return luaL_error(L, "error creating directory: %d", ret);
		}

		/* If no more slashes, we're done */
		if (!next_slash) {
			break;
		}
	}

	return 0;
}

/**
 * @brief Lua: require(module_name)
 *
 * Custom require function that loads from /lfs/
 * Loads and executes /lfs/<module_name>.lua once per VM, caching the result in
 * package.loaded as standard Lua does. Clear that entry to force a re-read.
 */
static int lua_file_require(lua_State *L)
{
	const char *module_name = luaL_checkstring(L, 1);

	/* Already loaded in this VM? Return the same value rather than running
	 * the file again, so a module's side effects happen once.
	 * Stack from here on: 1 = name, 2 = package, 3 = package.loaded.
	 */
	lua_getglobal(L, "package");
	lua_getfield(L, 2, "loaded");
	if (lua_getfield(L, 3, module_name) != LUA_TNIL) {
		return 1;
	}
	lua_pop(L, 1); /* the nil */

	const char *filename = lua_pushfstring(L, "%s.lua", module_name);

	/* Get full path */
	char full_path[256];
	int ret = get_full_path(filename, full_path, sizeof(full_path));
	if (ret < 0) {
		return luaL_error(L, "module name too long");
	}

	/* Open file */
	struct fs_file_t file;
	fs_file_t_init(&file);
	ret = fs_open(&file, full_path, FS_O_READ);
	if (ret < 0) {
		return luaL_error(L, "cannot open file: %s", filename);
	}

	/* Read entire file into buffer */
	size_t size = 0;
	size_t capacity = 1024;
	char *buffer = halo_malloc(capacity, HALO_MEM_REGION_AUTO);
	if (!buffer) {
		fs_close(&file);
		return luaL_error(L, "memory allocation error");
	}

	char ch;
	while (true) {
		ssize_t result = fs_read(&file, &ch, 1);
		if (result < 0) {
			halo_free(buffer);
			fs_close(&file);
			return luaL_error(L, "error reading file: %s", filename);
		}
		if (result == 0) {
			break; /* EOF */
		}

		/* Expand buffer if needed */
		if (size >= capacity) {
			capacity *= 2;
			char *new_buffer = halo_realloc(buffer, capacity, HALO_MEM_REGION_AUTO);
			if (!new_buffer) {
				halo_free(buffer);
				fs_close(&file);
				return luaL_error(L, "memory allocation error");
			}
			buffer = new_buffer;
		}

		buffer[size++] = ch;
	}

	fs_close(&file);

	/* Load and execute buffer */
	int status = luaL_loadbuffer(L, buffer, size, filename);
	halo_free(buffer);

	if (status != LUA_OK) {
		return luaL_error(L, "error loading module '%s': %s", module_name,
				  lua_tostring(L, -1));
	}

	/* Execute the module, keeping its first result -- as standard Lua does.
	 * Asking for exactly one result also means the `filename` string still on
	 * the stack can no longer be mistaken for the module's value, which is
	 * what the old fixed `return 1` handed back whenever a module returned
	 * nothing of its own.
	 */
	if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
		return luaL_error(L, "error executing module '%s': %s", module_name,
				  lua_tostring(L, -1));
	}

	/* A module that returns nothing is recorded as `true`. Lua does this so
	 * that "loaded" stays distinguishable from "not loaded yet"; leaving it
	 * nil would re-run the file on every require and defeat the cache.
	 */
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		lua_pushboolean(L, 1);
	}

	lua_pushvalue(L, -1);
	lua_setfield(L, 3, module_name); /* package.loaded[name] = result */

	return 1;
}

/**
 * @brief Service lifecycle event handler
 */
static int file_service_event_handler(halo_lua_event_t event, void *user_data)
{
	ARG_UNUSED(user_data);
	ARG_UNUSED(event);

	/* File service is always-on, no special handling needed */
	return 0;
}

/* Register service with lifecycle management (always-on service) */
HALO_LUA_SERVICE_DEFINE(file_service, file_service_event_handler, NULL, true);

/* File methods for userdata */
static const luaL_Reg file_methods[] = {
	{"read", lua_file_read},
	{"write", lua_file_write},
	{"close", lua_file_close},
	{NULL, NULL},
};

/* Metatable methods */
static const luaL_Reg meta_methods[] = {
	{"__index", NULL},           /* Will be set to file_methods table */
	{"__gc", lua_file_close},    /* Auto-close on garbage collection */
	{"__close", lua_file_close}, /* Close on explicit close */
	{NULL, NULL},
};

/**
 * @brief Open and register file library with Lua VM
 */
int lua_open_file_library(lua_State *L)
{

	/* Register service for lifecycle management */
	int ret = halo_lua_service_register(&file_service);
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("Failed to register file service: %d", ret);
		return ret;
	}

	/* Create file handle metatable */
	luaL_newmetatable(L, LUA_FILEHANDLE);
	luaL_setfuncs(L, meta_methods, 0);

	/* Create file methods table and set as __index */
	luaL_newlibtable(L, file_methods);
	luaL_setfuncs(L, file_methods, 0);
	lua_setfield(L, -2, "__index");

	lua_pop(L, 1); /* Pop metatable */

	/* Ensure main.lua exists */
	char main_lua_path[64];
	snprintf(main_lua_path, sizeof(main_lua_path), "%s/main.lua", halo_file_mount_point());

	struct fs_dirent stat;
	if (fs_stat(main_lua_path, &stat) != 0) {
		/* Create empty main.lua */
		struct fs_file_t file;
		fs_file_t_init(&file);
		ret = fs_open(&file, main_lua_path, FS_O_CREATE | FS_O_RDWR);
		if (ret == 0) {
			fs_close(&file);
			LOG_DBG("Created empty main.lua");
		} else {
			LOG_WRN("Failed to create main.lua: %d", ret);
		}
	}

	/* Get or create the global 'frame' table */
	lua_getglobal(L, "frame");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setglobal(L, "frame");
	}

	/* Create 'file' subtable */
	lua_newtable(L);

	/* Register file functions */
	lua_pushcfunction(L, lua_file_open);
	lua_setfield(L, -2, "open");

	lua_pushcfunction(L, lua_file_remove);
	lua_setfield(L, -2, "remove");

	lua_pushcfunction(L, lua_file_remove_all);
	lua_setfield(L, -2, "remove_all");

	lua_pushcfunction(L, lua_file_rename);
	lua_setfield(L, -2, "rename");

	lua_pushcfunction(L, lua_file_listdir);
	lua_setfield(L, -2, "listdir");

	lua_pushcfunction(L, lua_file_mkdir);
	lua_setfield(L, -2, "mkdir");

	/* Set 'file' table in 'frame' */
	lua_setfield(L, -2, "file");

	/* Pop frame table */
	lua_pop(L, 1);

	/* Override global 'require' function */
	lua_pushcfunction(L, lua_file_require);
	lua_setglobal(L, "require");

	/* package.loaded backs require()'s module cache. The package library
	 * itself is not opened (see lua_vm_init), so only 'loaded' exists --
	 * there are no searchers and no package.path. It is exposed rather than
	 * kept in the registry because clearing an entry is how Lua users force
	 * a module to be re-read, which matters on a device where a module can
	 * be re-uploaded between calls:
	 *     package.loaded['mymodule'] = nil
	 */
	lua_newtable(L);       /* package */
	lua_newtable(L);       /* package.loaded */
	lua_setfield(L, -2, "loaded");
	lua_setglobal(L, "package");

	LOG_DBG("File library registered successfully");
	return 0;
}
