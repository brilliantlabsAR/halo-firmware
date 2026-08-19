#pragma once
#include <stdint.h>
#include <stddef.h>
typedef void (*max98357a_audio_tx_tap_t)(const int16_t *pcm, size_t samples,
					 uint32_t sample_rate, uint8_t channels);
void max98357a_audio_set_tx_tap(max98357a_audio_tx_tap_t tap);
