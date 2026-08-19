/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * Lua log library — frame.log namespace
 *
 * Provides Lua access to the Zephyr LOG_BACKEND_FS log files:
 *   frame.log.list()         — print log file list to BLE terminal + return table
 *   frame.log.read(id)       — read a log file, return content as string
 *   frame.log.show(id)       — print log file content to BLE terminal
 *   frame.log.clear()        — delete all log files
 */

#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_backend_fs.h>
#include <zephyr/fs/fs.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include "halo/file_manager.h"
#include "halo/ble_lua.h"

LOG_MODULE_REGISTER(halo_lua_log, CONFIG_HALO_LOG_LEVEL);

#define LOG_PREFIX     CONFIG_LOG_BACKEND_FS_FILE_PREFIX
#define LOG_PREFIX_LEN (sizeof(CONFIG_LOG_BACKEND_FS_FILE_PREFIX) - 1)
#define FILE_NUM_LEN   4
#define MAX_PATH       256
#define MAX_LOG_FILES  32
#define BLE_CHUNK      200

static bool is_log_file(const char *name)
{
	size_t len = strlen(name);

	if (len != LOG_PREFIX_LEN + FILE_NUM_LEN) {
		return false;
	}

	if (memcmp(name, LOG_PREFIX, LOG_PREFIX_LEN) != 0) {
		return false;
	}

	for (int i = 0; i < FILE_NUM_LEN; i++) {
		if (name[LOG_PREFIX_LEN + i] < '0' ||
		    name[LOG_PREFIX_LEN + i] > '9') {
			return false;
		}
	}

	return true;
}

static int log_file_index(const char *name)
{
	return atoi(name + LOG_PREFIX_LEN);
}

static int build_path(const char *relative, char *buf, size_t size)
{
	const char *mnt = halo_file_mount_point();

	int ret = snprintf(buf, size, "%s/%s", mnt, relative);
	if (ret < 0 || ret >= size) {
		return -ENOMEM;
	}
	return 0;
}

static void ble_print(const char *str)
{
	halo_ble_lua_repl_write((const uint8_t *)str, strlen(str));
}

/**
 * @brief Get file size via seek + tell
 */
static ssize_t file_size(struct fs_file_t *fp)
{
	int ret = fs_seek(fp, 0, SEEK_END);
	if (ret < 0) {
		return ret;
	}

	off_t pos = fs_tell(fp);
	if (pos < 0) {
		return pos;
	}

	ret = fs_seek(fp, 0, SEEK_SET);
	if (ret < 0) {
		return ret;
	}

	return (ssize_t)pos;
}

/**
 * @brief Resolve filename argument from Lua stack
 *
 * Accepts number (5 → "log.0005") or string ("log.0005").
 * Writes result into filename buf (must be at least LOG_PREFIX_LEN + FILE_NUM_LEN + 1).
 */
static void resolve_filename(lua_State *L, char *filename)
{
	if (lua_type(L, 1) == LUA_TNUMBER) {
		int idx = luaL_checkinteger(L, 1);
		snprintf(filename, LOG_PREFIX_LEN + FILE_NUM_LEN + 1,
			 "%s%04d", LOG_PREFIX, idx);
	} else {
		const char *name = luaL_checkstring(L, 1);
		strncpy(filename, name, LOG_PREFIX_LEN + FILE_NUM_LEN);
		filename[LOG_PREFIX_LEN + FILE_NUM_LEN] = '\0';
	}
}

/**
 * @brief Lua: frame.log.list()
 *
 * Print log file list to BLE terminal AND return as Lua table.
 */
static int lua_log_list(lua_State *L)
{
	char dir_path[MAX_PATH];
	int ret = build_path("/", dir_path, sizeof(dir_path));
	if (ret < 0) {
		return luaL_error(L, "path too long");
	}

	struct fs_dir_t dir;
	fs_dir_t_init(&dir);

	ret = fs_opendir(&dir, dir_path);
	if (ret < 0) {
		ble_print("no log directory\n");
		lua_newtable(L);
		return 1;
	}

	typedef struct {
		char name[LOG_PREFIX_LEN + FILE_NUM_LEN + 1];
		ssize_t size;
		int index;
	} log_entry_t;

	log_entry_t entries[MAX_LOG_FILES];
	int count = 0;
	struct fs_dirent ent;

	while (count < MAX_LOG_FILES) {
		ret = fs_readdir(&dir, &ent);
		if (ret < 0 || ent.name[0] == '\0') {
			break;
		}

		if (ent.type != FS_DIR_ENTRY_FILE || !is_log_file(ent.name)) {
			continue;
		}

		strncpy(entries[count].name, ent.name, sizeof(entries[count].name) - 1);
		entries[count].name[sizeof(entries[count].name) - 1] = '\0';
		entries[count].size = ent.size;
		entries[count].index = log_file_index(ent.name);
		count++;
	}

	fs_closedir(&dir);

	/* Insertion sort by index */
	for (int i = 1; i < count; i++) {
		log_entry_t tmp = entries[i];
		int j = i - 1;
		while (j >= 0 && entries[j].index > tmp.index) {
			entries[j + 1] = entries[j];
			j--;
		}
		entries[j + 1] = tmp;
	}

	/* Print to BLE terminal */
	if (count == 0) {
		ble_print("no log files\n");
	} else {
		for (int i = 0; i < count; i++) {
			char line[80];
			snprintf(line, sizeof(line), "%.*s  %d bytes\n",
				 (int)(sizeof(entries[i].name) - 1),
				 entries[i].name, (int)entries[i].size);
			ble_print(line);
		}
	}

	/* Also return as Lua table */
	lua_newtable(L);
	for (int i = 0; i < count; i++) {
		lua_newtable(L);

		lua_pushstring(L, entries[i].name);
		lua_setfield(L, -2, "name");

		lua_pushinteger(L, entries[i].size);
		lua_setfield(L, -2, "size");

		lua_pushinteger(L, entries[i].index);
		lua_setfield(L, -2, "index");

		lua_seti(L, -2, i + 1);
	}

	return 1;
}

/**
 * @brief Lua: frame.log.read(id)
 *
 * Read a log file, return content as a Lua string.
 * id: number (5 → log.0005) or string ("log.0005")
 * Returns: string or nil
 */
static int lua_log_read(lua_State *L)
{
	char filename[LOG_PREFIX_LEN + FILE_NUM_LEN + 1];
	resolve_filename(L, filename);

	char full_path[MAX_PATH];
	int ret = build_path(filename, full_path, sizeof(full_path));
	if (ret < 0) {
		return luaL_error(L, "path too long");
	}

	struct fs_file_t file;
	fs_file_t_init(&file);

	ret = fs_open(&file, full_path, FS_O_READ);
	if (ret < 0) {
		lua_pushnil(L);
		return 1;
	}

	ssize_t size = file_size(&file);
	if (size <= 0) {
		fs_close(&file);
		lua_pushnil(L);
		return 1;
	}

	luaL_Buffer buf;
	char * const buf_ptr = luaL_buffinitsize(L, &buf, size);

	ssize_t bytes_read = fs_read(&file, buf_ptr, size);
	fs_close(&file);

	if (bytes_read < 0) {
		luaL_pushresultsize(&buf, 0);
		lua_pop(L, 1);
		lua_pushnil(L);
		return 1;
	}

	luaL_pushresultsize(&buf, bytes_read);
	return 1;
}

/**
 * @brief Lua: frame.log.show(id)
 *
 * Print a log file's content directly to the BLE terminal.
 * id: number (5 → log.0005) or string ("log.0005")
 * Returns: true on success, nil on failure
 */
static int lua_log_show(lua_State *L)
{
	char filename[LOG_PREFIX_LEN + FILE_NUM_LEN + 1];
	resolve_filename(L, filename);

	char full_path[MAX_PATH];
	int ret = build_path(filename, full_path, sizeof(full_path));
	if (ret < 0) {
		return luaL_error(L, "path too long");
	}

	struct fs_file_t file;
	fs_file_t_init(&file);

	ret = fs_open(&file, full_path, FS_O_READ);
	if (ret < 0) {
		ble_print("log file not found\n");
		lua_pushnil(L);
		return 1;
	}

	ssize_t size = file_size(&file);
	if (size <= 0) {
		fs_close(&file);
		ble_print("empty log file\n");
		lua_pushnil(L);
		return 1;
	}

	/* Print header */
	char header[64];
	snprintf(header, sizeof(header), "--- %s (%d bytes) ---\n",
		 filename, (int)size);
	ble_print(header);

	/* Stream content to BLE in chunks */
	char chunk[BLE_CHUNK];
	ssize_t remaining = size;

	while (remaining > 0) {
		ssize_t to_read = remaining > BLE_CHUNK ? BLE_CHUNK : remaining;
		ssize_t bytes = fs_read(&file, chunk, to_read);

		if (bytes <= 0) {
			break;
		}

		halo_ble_lua_repl_write((const uint8_t *)chunk, bytes);
		remaining -= bytes;
	}

	fs_close(&file);
	halo_ble_lua_repl_write((const uint8_t *)"\n", 1);

	lua_pushboolean(L, 1);
	return 1;
}

/**
 * @brief Lua: frame.log.clear()
 *
 * Delete all log files. Returns the number of files deleted.
 */
static int lua_log_clear(lua_State *L)
{
	char dir_path[MAX_PATH];
	int ret = build_path("/", dir_path, sizeof(dir_path));
	if (ret < 0) {
		return luaL_error(L, "path too long");
	}

	struct fs_dir_t dir;
	fs_dir_t_init(&dir);

	ret = fs_opendir(&dir, dir_path);
	if (ret < 0) {
		ble_print("no log directory\n");
		lua_pushinteger(L, 0);
		return 1;
	}

	int deleted = 0;
	struct fs_dirent ent;
	char file_path[MAX_PATH];

	while (true) {
		ret = fs_readdir(&dir, &ent);
		if (ret < 0 || ent.name[0] == '\0') {
			break;
		}

		if (ent.type != FS_DIR_ENTRY_FILE || !is_log_file(ent.name)) {
			continue;
		}

		ret = build_path(ent.name, file_path, sizeof(file_path));
		if (ret < 0) {
			continue;
		}

		ret = fs_unlink(file_path);
		if (ret == 0) {
			deleted++;
		}
	}

	fs_closedir(&dir);

	/* Reset the LOG_BACKEND_FS backend so it releases its handle to the
	 * deleted files and re-initializes on the next log write.
	 * Without this, the backend keeps operating on stale file handles and
	 * transitions to BACKEND_FS_CORRUPTED, permanently stopping log output.
	 */
	log_backend_fs_reset();

	char msg[64];
	snprintf(msg, sizeof(msg), "cleared %d log files\n", deleted);
	ble_print(msg);

	lua_pushinteger(L, deleted);
	return 1;
}

int lua_open_log_library(lua_State *L)
{
	lua_getglobal(L, "frame");

	lua_newtable(L);

	lua_pushcfunction(L, lua_log_list);
	lua_setfield(L, -2, "list");

	lua_pushcfunction(L, lua_log_read);
	lua_setfield(L, -2, "read");

	lua_pushcfunction(L, lua_log_show);
	lua_setfield(L, -2, "show");

	lua_pushcfunction(L, lua_log_clear);
	lua_setfield(L, -2, "clear");

	lua_setfield(L, -2, "log");

	lua_pop(L, 1);
	return 0;
}
