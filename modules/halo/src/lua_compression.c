/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <lz4.h>
#include "lua.h"
#include "lauxlib.h"
#include <halo/lua_compression.h>
#include <halo/lua_service.h>
#include <halo/mem_manager.h>

LOG_MODULE_REGISTER(lua_compression, CONFIG_HALO_LOG_LEVEL);

/* LZ4 Frame format magic numbers */
#define LZ4F_MAGICNUMBER         0x184D2204U
#define LZ4F_MAGIC_SKIPPABLE     0x184D2A50U
#define LZ4F_MIN_HEADER_SIZE     5
#define LZ4F_HEADER_SIZE_MIN     7

/**
 * @brief Compression callback state
 * 
 * Stores the Lua callback reference and decompressed data buffer.
 */
static struct {
	int callback_ref;                     /* LUA_REGISTRYINDEX reference */
	uint8_t buffer[4096];                 /* Decompressed data buffer */
	size_t buffer_size;                   /* Size of decompressed data */
	bool has_data;                        /* Flag indicating data is ready */
} decompress_state = {
	.callback_ref = LUA_NOREF,
	.buffer_size = 0,
	.has_data = false,
};

/**
 * @brief Read 32-bit little-endian value
 */
static uint32_t read_le32(const void *src)
{
	const uint8_t *p = (const uint8_t *)src;
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | 
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * @brief Get LZ4 frame header size
 * 
 * @param src Source data
 * @param src_size Source data size
 * @return Header size, or negative error code
 */
static int get_header_size(const void *src, size_t src_size)
{
	/* Check minimum size */
	if (src_size < LZ4F_MIN_HEADER_SIZE) {
		return -20;
	}

	uint32_t magic = read_le32(src);

	/* Check for skippable frame */
	if ((magic & 0xFFFFFFF0U) == LZ4F_MAGIC_SKIPPABLE) {
		return 8;
	}

	/* Verify magic number */
	if (magic != LZ4F_MAGICNUMBER) {
		return -21;
	}

	/* Parse frame header */
	const uint8_t *p = (const uint8_t *)src;
	uint8_t flg = p[4];
	uint32_t content_size_flag = (flg >> 3) & 0x01;
	uint32_t dict_id_flag = flg & 0x01;

	return LZ4F_HEADER_SIZE_MIN + (content_size_flag ? 8 : 0) + (dict_id_flag ? 4 : 0);
}

/**
 * @brief Lua hook handler for decompression callback
 * 
 * Called asynchronously after each block is decompressed.
 */
static void decompression_hook_handler(lua_State *L, lua_Debug *ar)
{
	ARG_UNUSED(ar);
	
	/* Clear hook immediately */
	lua_sethook(L, NULL, 0, 0);

	if (decompress_state.callback_ref == LUA_NOREF) {
		LOG_WRN("Decompression complete but no callback registered");
		decompress_state.has_data = false;
		return;
	}

	if (!decompress_state.has_data) {
		LOG_WRN("Hook called but no data available");
		return;
	}

	/* Get callback function from registry */
	lua_rawgeti(L, LUA_REGISTRYINDEX, decompress_state.callback_ref);

	/* Push decompressed data as string */
	lua_pushlstring(L, (const char *)decompress_state.buffer, 
	                decompress_state.buffer_size);

	/* Clear data flag */
	decompress_state.has_data = false;

	/* Call callback */
	if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
		const char *error = lua_tostring(L, -1);
		LOG_ERR("Decompression callback error: %s", error);
		lua_pop(L, 1);
	}
}

/**
 * @brief Process decompressed block callback
 * 
 * Called by decompression function for each decompressed block.
 * Stores data and triggers Lua hook.
 */
static void process_decompressed_block(void *context, void *data, size_t data_size)
{
	lua_State *L = (lua_State *)context;

	if (data_size > sizeof(decompress_state.buffer)) {
		LOG_ERR("Decompressed block too large: %u > %u", 
		        data_size, sizeof(decompress_state.buffer));
		data_size = sizeof(decompress_state.buffer);
	}

	/* Copy decompressed data */
	memcpy(decompress_state.buffer, data, data_size);
	decompress_state.buffer_size = data_size;
	decompress_state.has_data = true;

	/* Trigger Lua callback via hook */
	lua_sethook(L, decompression_hook_handler,
	           LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE | LUA_MASKCOUNT, 1);
}

/**
 * @brief Decompress LZ4 compressed data
 * 
 * @param dest_size Destination block size
 * @param source Compressed source data
 * @param source_size Compressed data size
 * @param callback Callback function for each decompressed block
 * @param context Callback context (Lua state)
 * @return 0 on success, negative error code on failure
 */
static int decompress_lz4(size_t dest_size, const void *source, size_t source_size,
                          void (*callback)(void *, void *, size_t), void *context)
{
	int status = 0;

	/* Get frame header size */
	int header_size = get_header_size(source, source_size);
	if (header_size < 0) {
		LOG_ERR("Invalid LZ4 frame header: %d", header_size);
		return header_size;
	}

	/* Allocate output buffer using halo memory manager */
	char *output = halo_malloc(dest_size, HALO_MEM_REGION_AUTO);
	if (!output) {
		LOG_ERR("Failed to allocate %u bytes for decompression", dest_size);
		return -ENOMEM;
	}

	/* Process blocks */
	const char *block_ptr = (const char *)source + header_size;
	const char *source_end = (const char *)source + source_size;

	while (block_ptr < source_end) {
		/* Check remaining space */
		if (block_ptr + 4 > source_end) {
			LOG_WRN("Incomplete block header at end of data");
			break;
		}

		/* Read block size (little-endian) */
		uint32_t block_size = read_le32(block_ptr);

		/* End of frame marker */
		if (block_size == 0) {
			status = 0;
			break;
		}

		/* Check block size validity */
		if (block_ptr + 4 + block_size > source_end) {
			LOG_ERR("Block size %u exceeds remaining data", block_size);
			status = -EINVAL;
			break;
		}

		/* Decompress block */
		status = LZ4_decompress_safe(block_ptr + 4, output, 
		                              block_size, dest_size);

		if (status <= 0) {
			LOG_ERR("LZ4_decompress_safe failed: %d", status);
			break;
		}

		/* Call callback with decompressed data */
		callback(context, output, status);

		/* Move to next block */
		block_ptr += block_size + 4;
	}

	halo_free(output);
	return status;
}

/**
 * @brief Lua: frame.compression.process_function(callback)
 * 
 * Register or clear callback for decompressed data.
 * - Pass function to register callback
 * - Pass nil to clear callback
 */
static int lua_compression_process_function(lua_State *L)
{
	/* Check if clearing callback */
	if (lua_isnil(L, 1)) {
		if (decompress_state.callback_ref != LUA_NOREF) {
			luaL_unref(L, LUA_REGISTRYINDEX, decompress_state.callback_ref);
			decompress_state.callback_ref = LUA_NOREF;
			decompress_state.has_data = false;
		}
		LOG_DBG("Compression process function cleared");
		return 0;
	}

	/* Must be a function */
	if (!lua_isfunction(L, 1)) {
		return luaL_error(L, "expected function or nil");
	}

	/* Unref old callback if exists */
	if (decompress_state.callback_ref != LUA_NOREF) {
		luaL_unref(L, LUA_REGISTRYINDEX, decompress_state.callback_ref);
	}

	/* Store new callback */
	decompress_state.callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	decompress_state.has_data = false;
	LOG_DBG("Compression process function registered");
	return 0;
}

/**
 * @brief Lua: frame.compression.decompress(data, block_size)
 * 
 * Decompress LZ4 compressed data.
 * Calls process_function callback for each decompressed block.
 * 
 * @param L Lua state
 * @return 0 on success, error on failure
 */
static int lua_compression_decompress(lua_State *L)
{
	size_t data_len;
	const char *data = luaL_checklstring(L, 1, &data_len);
	lua_Integer block_size = luaL_checkinteger(L, 2);

	LOG_DBG("Compression decompress: %zu bytes, block_size %lld", data_len, block_size);

	if (block_size <= 0) {
		return luaL_error(L, "block_size must be greater than 0");
	}

	if (block_size > 1024 * 1024) {  /* 1MB limit */
		return luaL_error(L, "block_size too large (max 1MB)");
	}

	if (decompress_state.callback_ref == LUA_NOREF) {
		return luaL_error(L, "no process_function registered");
	}

	int ret = decompress_lz4(block_size, data, data_len, 
	                         process_decompressed_block, L);

	if (ret < 0) {
		return luaL_error(L, "decompression failed: %d", ret);
	}

	return 0;
}

/**
 * @brief Service lifecycle event handler
 */
static int compression_service_event_handler(halo_lua_event_t event, void *user_data)
{
	ARG_UNUSED(user_data);
	
	switch (event) {
	case HALO_LUA_EVENT_INIT:
		decompress_state.callback_ref = LUA_NOREF;
		decompress_state.has_data = false;
		break;
		
	case HALO_LUA_EVENT_DEINIT:
		decompress_state.callback_ref = LUA_NOREF;
		decompress_state.has_data = false;
		break;
		
	case HALO_LUA_EVENT_INTERRUPT:
		/* Clear callback on Ctrl+C */
		decompress_state.callback_ref = LUA_NOREF;
		decompress_state.has_data = false;
		break;
	
	default:
		break;
	}
	
	return 0;
}

/* Register service with lifecycle management (always-on service) */
HALO_LUA_SERVICE_DEFINE(compression_service, compression_service_event_handler, NULL, true);

/**
 * @brief Open and register compression library with Lua VM
 */
int lua_open_compression_library(lua_State *L)
{
	/* Register service for lifecycle management */
	int ret = halo_lua_service_register(&compression_service);
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("Failed to register compression service: %d", ret);
		return ret;
	}

	/* Get or create the global 'frame' table */
	lua_getglobal(L, "frame");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setglobal(L, "frame");
	}

	/* Create 'compression' subtable */
	lua_newtable(L);

	/* Register compression functions */
	lua_pushcfunction(L, lua_compression_process_function);
	lua_setfield(L, -2, "process_function");

	lua_pushcfunction(L, lua_compression_decompress);
	lua_setfield(L, -2, "decompress");

	/* Set 'compression' table in 'frame' */
	lua_setfield(L, -2, "compression");

	/* Pop frame table */
	lua_pop(L, 1);

	LOG_DBG("Compression library registered successfully");
	return 0;
}
