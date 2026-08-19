/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_BLE_BATTERY_H
#define HALO_BLE_BATTERY_H

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize BLE Battery Service (0x180F)
 * 
 * Creates a standard BLE Battery Service using generic GATT API.
 * Integrates with battery_manager for automatic level updates.
 * 
 * @param reset If true, reset the service state
 * @return 0 on success, negative error code on failure
 */
int halo_ble_battery_init(bool reset);

/**
 * @brief Deinitialize BLE Battery Service
 * 
 * @return 0 on success, negative error code on failure
 */
int halo_ble_battery_deinit(void);

/**
 * @brief Manually send battery level notification
 * 
 * Normally notifications are sent automatically when battery level changes.
 * This function allows manual notification trigger.
 * 
 * @param level Battery level (0-100%)
 * @return 0 on success, negative error code on failure
 */
int halo_ble_battery_notify(uint8_t level);

/**
 * @brief Check if battery notifications are enabled
 * 
 * @return true if client has enabled notifications, false otherwise
 */
bool halo_ble_battery_is_notify_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* HALO_BLE_BATTERY_H */
