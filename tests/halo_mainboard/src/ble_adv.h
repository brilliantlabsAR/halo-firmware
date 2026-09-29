/*
 * Copyright (c) 2026 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MBT_BLE_ADV_H_
#define MBT_BLE_ADV_H_

#include <stdint.h>
#include <zephyr/kernel.h>

/*
 * Bring up the BLE stack and advertise `name` (connectable, general
 * discoverable, 100 ms interval, indefinitely) from static identity address
 * `addr`, given most-significant byte first. Each stack step waits at most
 * `step_timeout`. Call once per boot.
 */
int mbt_ble_adv_start(const char *name, const uint8_t addr[6], k_timeout_t step_timeout);

#endif
