/*
 * Copyright (c) 2026 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MBT_BLE_ADV_H_
#define MBT_BLE_ADV_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>

/*
 * Bring up the BLE stack and advertise `name` (connectable, general
 * discoverable, 100 ms interval, indefinitely) from static identity address
 * `addr`, given most-significant byte first. Each stack step waits at most
 * `step_timeout`. Call once per boot.
 */
int mbt_ble_adv_start(const char *name, const uint8_t addr[6], k_timeout_t step_timeout);

/* True from a successful start until the stack reports the set stopped */
bool mbt_ble_adv_running(void);

/*
 * Round-trip to the controller (HCI version read) to show the stack is still
 * alive. Returns 0 and the controller's HCI version on success.
 */
int mbt_ble_ping(k_timeout_t timeout, uint8_t *hci_ver, uint16_t *hci_subver);

#endif
