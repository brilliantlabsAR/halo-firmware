/**
 * Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 *
 * @details
 * This header file contains API for translation between the BLE host stack
 * and the audio datapath (audio queues and ISO data paths).
 * @note
 * This API is currently only available for LE Audio usage.
 *
 */

#ifndef AUDIO_UTILS_H__
#define AUDIO_UTILS_H__

#include <inttypes.h>
#include <zephyr/types.h>
#include <bap.h>
#include <lc3_api.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Converts from a uint32_t sampling rate to enum bap_sampling_freq
 *
 * @param[in] rate The sampling rate in Hz to convert
 *
 * @return The corresponding enum bap_sampling_freq value.
 */
enum bap_sampling_freq audio_hz_to_bap_sampling_freq(uint32_t rate);

/**
 * @brief Converts from enum bap_sampling_freq to a uint32_t sampling rate
 *
 * @param[in] freq The enum bap_sampling_freq value to convert
 *
 * @return The corresponding sampling rate in Hz.
 */
uint32_t audio_bap_sampling_freq_to_hz(enum bap_sampling_freq freq);

/**
 * @brief Converts from enum bap_frame_dur to lc3_frame_duration_t.
 *
 * @param[in] dur The enum bap_frame_dur value to convert
 *
 * @return The corresponding lc3_frame_duration_t value.
 */
lc3_frame_duration_t audio_bap_frame_dur_to_lc3_frame_dur(enum bap_frame_dur dur);

/**
 * @brief Converts from lc3_frame_duration_t to enum bap_frame_dur.
 *
 * @param[in] dur The lc3_frame_duration_t value to convert
 *
 * @return The corresponding enum bap_frame_dur value.
 */
enum bap_frame_dur audio_lc3_frame_dur_to_bap_frame_dur(lc3_frame_duration_t dur);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_UTILS_H__ */
