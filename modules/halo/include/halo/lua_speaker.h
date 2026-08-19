/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_LUA_SPEAKER_H
#define HALO_LUA_SPEAKER_H

#include "lua.h"

/**
 * @file lua_speaker.h
 * @brief Lua speaker library for audio playback
 * 
 * Provides frame.speaker.* API for playing audio from BLE.
 * Supports both PCM and LC3 formats with automatic decoding.
 */

/**
 * @brief Open and register speaker library with Lua VM
 * 
 * Registers frame.speaker table with the following functions:
 * - start({encoder, sample_rate, bitrate, duration, volume})
 * - play(data) - compatibility, no-op in new architecture
 * - stop()
 * - volume(level)
 * 
 * @param L Lua state
 * @return 0 on success
 */
int lua_open_speaker_library(lua_State *L);

/**
 * @brief Stop frame.speaker streaming and release the speaker immediately.
 *
 * Used on the way into deep sleep: long-lived PCM/LC3 streams (e.g. assistant
 * voice) otherwise hold the speaker singleton, which would silence the
 * shutdown sound and leave the pump thread to be force-aborted mid-write when
 * the Lua runtime is torn down. Safe to call from any thread; does not touch
 * the Lua VM. Waits a bounded time for the pump loop to drain.
 */
void halo_lua_speaker_interrupt(void);

#endif /* HALO_LUA_SPEAKER_H */
