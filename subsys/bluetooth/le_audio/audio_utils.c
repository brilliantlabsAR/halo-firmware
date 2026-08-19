/**
 * Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/logging/log.h>
#include <zephyr/types.h>
#include <zephyr/sys/__assert.h>

#include <stdint.h>
#include "audio_utils.h"
#include "gaf.h"
#include "bap.h"
#include "lc3_api.h"

LOG_MODULE_REGISTER(audio_tofromh, CONFIG_BLE_AUDIO_LOG_LEVEL);

uint32_t audio_bap_sampling_freq_to_hz(enum bap_sampling_freq const freq)
{
	switch (freq) {
	case BAP_SAMPLING_FREQ_8000HZ: /* BAP_SAMPLING_FREQ_MIN implicitly set to 8000Hz */
		return 8000;
	case BAP_SAMPLING_FREQ_11025HZ:
		return 11025;
	case BAP_SAMPLING_FREQ_16000HZ:
		return 16000;
	case BAP_SAMPLING_FREQ_22050HZ:
		return 22050;
	case BAP_SAMPLING_FREQ_24000HZ:
		return 24000;
	case BAP_SAMPLING_FREQ_32000HZ:
		return 32000;
	case BAP_SAMPLING_FREQ_44100HZ:
		return 44100;
	case BAP_SAMPLING_FREQ_48000HZ:
		return 48000;
	case BAP_SAMPLING_FREQ_88200HZ:
		return 88200;
	case BAP_SAMPLING_FREQ_96000HZ:
		return 96000;
	case BAP_SAMPLING_FREQ_176400HZ:
		return 176400;
	case BAP_SAMPLING_FREQ_192000HZ:
		return 192000;
	case BAP_SAMPLING_FREQ_384000HZ: /* BAP_SAMPLING_FREQ_MAX implicitly set to 384000Hz */
		return 384000;
	default:
		__ASSERT(false, "Unknown sampling frequency");
		LOG_ERR("Unknown sampling frequency");
		return 0;
	}
}

enum bap_sampling_freq audio_hz_to_bap_sampling_freq(uint32_t const rate)
{
	switch (rate) {
	case 8000:
		return BAP_SAMPLING_FREQ_8000HZ;
	case 11025:
		return BAP_SAMPLING_FREQ_11025HZ;
	case 16000:
		return BAP_SAMPLING_FREQ_16000HZ;
	case 22050:
		return BAP_SAMPLING_FREQ_22050HZ;
	case 24000:
		return BAP_SAMPLING_FREQ_24000HZ;
	case 32000:
		return BAP_SAMPLING_FREQ_32000HZ;
	case 44100:
		return BAP_SAMPLING_FREQ_44100HZ;
	case 48000:
		return BAP_SAMPLING_FREQ_48000HZ;
	case 88200:
		return BAP_SAMPLING_FREQ_88200HZ;
	case 96000:
		return BAP_SAMPLING_FREQ_96000HZ;
	case 176400:
		return BAP_SAMPLING_FREQ_176400HZ;
	case 192000:
		return BAP_SAMPLING_FREQ_192000HZ;
	case 384000:
		return BAP_SAMPLING_FREQ_384000HZ;
	default:
		__ASSERT(false, "Unknown sampling frequency");
		LOG_ERR("Unknown sampling frequency");
		return BAP_SAMPLING_FREQ_UNKNOWN;
	}
}

lc3_frame_duration_t audio_bap_frame_dur_to_lc3_frame_dur(enum bap_frame_dur dur)
{
	switch (dur) {
	case BAP_FRAME_DUR_7_5MS:
		return FRAME_DURATION_7_5_MS;
	case BAP_FRAME_DUR_10MS:
		return FRAME_DURATION_10_MS;
	default:
		__ASSERT(false, "Unknown frame duration");
		LOG_ERR("Unknown frame duration");
		return 0;
	}
}

enum bap_frame_dur audio_lc3_frame_dur_to_bap_frame_dur(lc3_frame_duration_t dur)
{
	switch (dur) {
	case FRAME_DURATION_7_5_MS:
		return BAP_FRAME_DUR_7_5MS;
	case FRAME_DURATION_10_MS:
		return BAP_FRAME_DUR_10MS;
	default:
		__ASSERT(false, "Unknown frame duration");
		LOG_ERR("Unknown frame duration");
		return 0;
	}
}
