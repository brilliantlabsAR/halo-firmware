/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 *
 */

/**
 * @file utmr.h
 *
 */

#ifndef UTMR_H
#define UTMR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void utimer_init(void);

void utimer_start(void);
void utimer_stop(void);

uint32_t utimer_get_s(void);
uint32_t utimer_get_ms(void);
uint32_t utimer_get_us(void);
uint64_t utimer_get_ns(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*UTMR_H*/
