/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_BLE_LUA_H
#define HALO_BLE_LUA_H

#include <stdint.h>
#include <stddef.h>
#include <zephyr/kernel.h>

/**
 * @file ble_lua.h
 * @brief BLE Lua Service for script execution and data transport
 * 
 * This service provides multiple characteristics for Lua script communication:
 * - RX/TX: REPL command input/output and data transfer (Write Without Response)
 * - Video: Video stream transmission (Notify)
 * - Audio RX/TX: Audio input/output (Write Without Response + Notify)
 * 
 * RX characteristics use Write Without Response for better throughput and lower latency.
 */

/* Lua Service UUID: 7A230001-5475-A6A4-654C-8431F6AD49C4 */
#define HALO_LUA_UUID_128_SVC \
	{0xC4, 0x49, 0xAD, 0xF6, 0x31, 0x84, 0x4C, 0x65, \
	 0xA4, 0xA6, 0x75, 0x54, 0x01, 0x00, 0x23, 0x7A}

/* RX Characteristic UUID: 7A230002-5475-A6A4-654C-8431F6AD49C4 */
#define HALO_LUA_UUID_128_RX \
	{0xC4, 0x49, 0xAD, 0xF6, 0x31, 0x84, 0x4C, 0x65, \
	 0xA4, 0xA6, 0x75, 0x54, 0x02, 0x00, 0x23, 0x7A}

/* TX Characteristic UUID: 7A230003-5475-A6A4-654C-8431F6AD49C4 */
#define HALO_LUA_UUID_128_TX \
	{0xC4, 0x49, 0xAD, 0xF6, 0x31, 0x84, 0x4C, 0x65, \
	 0xA4, 0xA6, 0x75, 0x54, 0x03, 0x00, 0x23, 0x7A}

/* Video Characteristic UUID: 7A230004-5475-A6A4-654C-8431F6AD49C4 */
#define HALO_LUA_UUID_128_VIDEO \
	{0xC4, 0x49, 0xAD, 0xF6, 0x31, 0x84, 0x4C, 0x65, \
	 0xA4, 0xA6, 0x75, 0x54, 0x04, 0x00, 0x23, 0x7A}

/* Audio RX Characteristic UUID: 7A230005-5475-A6A4-654C-8431F6AD49C4 */
#define HALO_LUA_UUID_128_AUDIO_RX \
	{0xC4, 0x49, 0xAD, 0xF6, 0x31, 0x84, 0x4C, 0x65, \
	 0xA4, 0xA6, 0x75, 0x54, 0x05, 0x00, 0x23, 0x7A}

/* Audio TX Characteristic UUID: 7A230006-5475-A6A4-654C-8431F6AD49C4 */
#define HALO_LUA_UUID_128_AUDIO_TX \
	{0xC4, 0x49, 0xAD, 0xF6, 0x31, 0x84, 0x4C, 0x65, \
	 0xA4, 0xA6, 0x75, 0x54, 0x06, 0x00, 0x23, 0x7A}

/* Control codes for Lua execution */
#define HALO_LUA_CTRL_DATA_MARKER  0x01  /* Data transfer marker (first byte) */
#define HALO_LUA_CTRL_REBOOT       0x02  /* Ctrl+B: Reboot device */
#define HALO_LUA_CTRL_INTERRUPT    0x03  /* Ctrl+C: Interrupt script execution */
#define HALO_LUA_CTRL_RESTART      0x04  /* Ctrl+D: Restart Lua runtime */
#define HALO_LUA_CTRL_RESET        0x05  /* Ctrl+E: Reset and remove main.lua */
#define HALO_LUA_CTRL_EXIT         0x06  /* Ctrl+F: Exit Lua runtime completely */
#define HALO_LUA_CTRL_REMOVE_ALL   0x07  /* Ctrl+G: Remove all files/folders (except settings) */

/**
 * @brief Lua control command handler callback
 *
 * This callback is invoked when control commands are received from BLE client:
 * - 0x03 (Ctrl+C): Interrupt script execution
 * - 0x04 (Ctrl+D): Restart Lua runtime
 * - 0x05 (Ctrl+E): Reset and remove main.lua
 * - 0x06 (Ctrl+F): Exit Lua runtime completely
 * - 0x07 (Ctrl+G): Remove all files/folders (except settings)
 *
 * @param ctrl_code Control code received
 */
typedef void (*halo_ble_lua_ctrl_handler_t)(uint8_t ctrl_code);

/**
 * @brief Initialize the BLE Lua service
 * 
 * @param reset If true, reset the service state
 * @return int 0 on success, negative error code otherwise
 */
int halo_ble_lua_init(bool reset);

/**
 * @brief Deinitialize the BLE Lua service
 * 
 * @return int 0 on success, negative error code otherwise
 */
int halo_ble_lua_deinit(void);

/**
 * @brief Read data from the Lua REPL input
 * 
 * @param data Buffer to store read data
 * @param len Maximum length to read
 * @param timeout Timeout for the operation
 * @return int32_t Number of bytes read, 0 on timeout, negative on error
 */
int32_t halo_ble_lua_repl_read(uint8_t *data, size_t len, k_timeout_t timeout);

/**
 * @brief Write data to the Lua REPL output
 * 
 * @param data Data to write
 * @param len Length of data
 * @return int32_t Number of bytes written, negative on error
 */
int32_t halo_ble_lua_repl_write(const uint8_t *data, size_t len);

/**
 * @brief Read binary data from the data channel
 * 
 * @param data Buffer to store read data
 * @param len Maximum length to read
 * @param timeout Timeout for the operation
 * @return int32_t Number of bytes read, 0 on timeout, negative on error
 */
int32_t halo_ble_lua_data_read(uint8_t *data, size_t len, k_timeout_t timeout);

/**
 * @brief Write binary data to the data channel
 * 
 * @param data Data to write
 * @param len Length of data
 * @return int32_t Number of bytes written, negative on error
 */
int32_t halo_ble_lua_data_write(const uint8_t *data, size_t len);

/**
 * @brief Write video data to the video channel
 * 
 * @param data Video data to write
 * @param len Length of data
 * @return int32_t Number of bytes written, negative on error
 */
int32_t halo_ble_lua_video_write(const uint8_t *data, size_t len);

/**
 * @brief Read audio data from the audio input channel
 * 
 * @param data Buffer to store read data
 * @param len Maximum length to read
 * @param timeout Timeout for the operation
 * @return int32_t Number of bytes read, 0 on timeout, negative on error
 */
int32_t halo_ble_lua_audio_read(uint8_t *data, size_t len, k_timeout_t timeout);

/**
 * @brief Write audio data to the audio output channel
 * 
 * @param data Audio data to write
 * @param len Length of data
 * @return int32_t Number of bytes written, negative on error
 */
int32_t halo_ble_lua_audio_write(const uint8_t *data, size_t len);

/**
 * @brief Register a callback for Lua control commands
 * 
 * This callback will be invoked when control commands like Ctrl+C or Ctrl+Z
 * are received from the BLE client.
 * 
 * @param handler Callback function, or NULL to unregister
 */
void halo_ble_lua_register_ctrl_handler(halo_ble_lua_ctrl_handler_t handler);

#ifdef CONFIG_HALO_AUDIO_STREAM
/**
 * @brief Register BLE Lua audio transport with audio_stream
 * 
 * Registers BLE Lua service as both an audio source (for speaker) and
 * audio sink (for microphone). This enables Lua scripts to use audio
 * via BLE transport.
 * 
 * Should be called during initialization if BLE audio is enabled.
 * 
 * 
 * 
 * @return 0 on success, negative errno on failure
 */
int halo_ble_lua_audio_transport_register(void);

/**
 * @brief Unregister BLE Lua audio transport
 * 
 * Removes BLE Lua service from audio_stream sources and sinks.
 * Should be called during deinitialization.
 * 
 * @return 0 on success, negative errno on failure
 */
int halo_ble_lua_audio_transport_unregister(void);
#endif /* CONFIG_HALO_AUDIO_STREAM */

#endif /* HALO_BLE_LUA_H */
