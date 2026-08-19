/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef AUDIOBACKEND_H
#define AUDIOBACKEND_H

#include <stdint.h>

int audio_init(int sampling_rate);
void audio_uninit(void);
int get_audio_data(int16_t *data, int len);
int wait_for_audio(void);
void audio_preprocessing(int16_t *data, int len);

#endif
