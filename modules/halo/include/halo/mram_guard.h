/* Copyright (c) 2026 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_MRAM_GUARD_H
#define HALO_MRAM_GUARD_H

#include <stdbool.h>
#include <stdint.h>

/** 0 when the MPU guard over the boot partition and SE area is active. */
int halo_mram_guard_status(void);

/** Raw TT (test target) result for an address: MPU region and permissions. */
uint32_t halo_mram_tt(uint32_t addr);

/** True when a privileged store to addr would reach the bus. */
bool halo_mram_writable(uint32_t addr);

#endif
