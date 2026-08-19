/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_BLE_CONNECTION_H
#define HALO_BLE_CONNECTION_H

#include <zephyr/kernel.h>
#include "halo/ble_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize connection manager
 * 
 * @param device_name Device name (NULL or empty for auto-generated "Halo XX" from EUI48)
 * @return 0 on success, negative error code on failure
 */
int halo_ble_conn_init(const char *device_name);

/**
 * @brief Get the device's BLE name (e.g. "Halo AB")
 *
 * Valid after halo_ble_conn_init(); returns an empty string before that.
 *
 * @return NUL-terminated device name
 */
const char *halo_ble_conn_get_device_name(void);

/**
 * @brief Start advertising
 * 
 * @param params Advertising parameters (NULL for defaults)
 * @return 0 on success, negative error code on failure
 */
int halo_ble_conn_adv_start(const struct halo_ble_adv_params *params);

/**
 * @brief Stop advertising
 * 
 * @return 0 on success, negative error code on failure
 */
int halo_ble_conn_adv_stop(void);

/**
 * @brief Disconnect current connection
 * 
 * @return 0 on success, negative error code on failure
 */
int halo_ble_conn_disconnect(void);

/**
 * @brief Check if connected
 * 
 * @return true if connected, false otherwise
 */
bool halo_ble_conn_is_connected(void);

/**
 * @brief Check if advertising
 * 
 * @return true if advertising, false otherwise
 */
bool halo_ble_conn_is_advertising(void);

/**
 * @brief Get connection index
 * 
 * @return Connection index
 */
uint8_t halo_ble_conn_get_conidx(void);

/**
 * @brief Get MTU size
 * 
 * @return MTU size in bytes
 */
uint16_t halo_ble_conn_get_mtu(void);

/**
 * @brief Get device address
 * 
 * @param addr Buffer to store address (6 bytes)
 * @return 0 on success, negative error code on failure
 */
int halo_ble_conn_get_address(uint8_t addr[6]);

/**
 * @brief Update connection parameters
 * 
 * @param params Connection parameters
 * @return 0 on success, negative error code on failure
 */
int halo_ble_conn_update_params(const struct halo_ble_conn_params *params);

/**
 * @brief Update activity timestamp
 */
void halo_ble_conn_update_activity(void);

/**
 * @brief Get last activity timestamp
 * 
 * @return Timestamp in milliseconds
 */
uint32_t halo_ble_conn_get_last_activity(void);

/**
 * @brief Handle connection event (internal, called by Alif BLE callbacks)
 * 
 * @param conidx Connection index
 * @param mtu MTU size
 */
void halo_ble_conn_on_connected(uint8_t conidx, uint16_t mtu);

/**
 * @brief Handle disconnection event (internal, called by Alif BLE callbacks)
 * 
 * @param conidx Connection index
 * @param reason Disconnection reason
 */
void halo_ble_conn_on_disconnected(uint8_t conidx, uint16_t reason);

/**
 * @brief Handle MTU change event (internal, called by Alif BLE callbacks)
 * 
 * @param conidx Connection index
 * @param mtu New MTU size
 */
void halo_ble_conn_on_mtu_changed(uint8_t conidx, uint16_t mtu);

/**
 * @brief Prepare for system reboot by clearing BLE noinit state.
 *
 * Clears both the connection context init magic and the alif_ble initialised
 * flag so that the next boot performs a full cold BLE start. Must be called
 * before every sys_reboot() to prevent the warm-restart path from skipping
 * gapm_configure() and BLE advertising after OTA or explicit reboots.
 */
void halo_ble_conn_prepare_reboot(void);


#endif /* HALO_BLE_CONNECTION_H */
