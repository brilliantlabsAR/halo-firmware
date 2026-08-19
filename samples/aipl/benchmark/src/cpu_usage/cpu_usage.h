/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 *
 */

/******************************************************************************
 * @file     cpu_usage.h
 * @brief    CPU usage calculation hooks
 *
 ******************************************************************************/
#ifndef CPU_USAGE_H
#define CPU_USAGE_H

#include <stdint.h>

void cpu_usage_enable(void);

uint32_t zephyr_get_idle_time(void);

#endif /* CPU_USAGE_H */
