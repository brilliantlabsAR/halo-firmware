/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_BLE_SECURITY_H
#define HALO_BLE_SECURITY_H

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>

/* Alif BLE includes */
#include "gap_le.h"
#include "gapc.h"
#include "gapc_sec.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bond information structure
 */
struct halo_ble_bond_info {
	gap_bdaddr_t peer_addr;
	gapc_pairing_keys_t keys;
	gapc_bond_data_t data;
	uint32_t last_used; /* Monotonic sequence for LRU eviction */
	bool valid;
};

/**
 * @brief Initialize security manager
 * 
 * @return 0 on success, negative error code on failure
 */
int halo_ble_sec_init(void);

/**
 * @brief Handle pairing request
 * 
 * @param conidx Connection index
 * @param auth_level Authentication level
 * @return 0 on success, negative error code on failure
 */
int halo_ble_sec_pair_request(uint8_t conidx, uint8_t auth_level);

/**
 * @brief Accept pairing
 * 
 * @param conidx Connection index
 * @return 0 on success, negative error code on failure
 */
int halo_ble_sec_pair_accept(uint8_t conidx);

/**
 * @brief Reject pairing
 * 
 * @param conidx Connection index
 * @return 0 on success, negative error code on failure
 */
int halo_ble_sec_pair_reject(uint8_t conidx);

/**
 * @brief Clear ALL bonded devices
 *
 * @return 0 on success, negative error code on failure
 */
int halo_ble_sec_bond_clear(void);

/**
 * @brief Open the pairing window
 *
 * For CONFIG_HALO_BLE_PAIRING_WINDOW_SEC seconds the device accepts and bonds
 * one new (unknown) peer, evicting the least-recently-used bond if the table
 * is full. Already-bonded peers are rejected while the window is open so the
 * single connection stays free for the new device. Re-opening restarts the
 * timer. See PAIRING.md.
 *
 * @return 0 on success, negative error code on failure
 */
int halo_ble_sec_pairing_window_open(void);

/**
 * @brief Check whether the pairing window is currently open
 *
 * @return true if open
 */
bool halo_ble_sec_pairing_window_is_open(void);

/**
 * @brief Check if device is bonded
 * 
 * @return true if bonded, false otherwise
 */
bool halo_ble_sec_is_bonded(void);

/**
 * @brief Check if device is paired
 * 
 * @return true if paired, false otherwise
 */
bool halo_ble_sec_is_paired(void);

/**
 * @brief Check if the current link is encrypted
 *
 * Returns true only once the controller has reported encryption established
 * (auth_info), for both fresh pairing and bonded reconnection. This is stricter
 * than halo_ble_sec_is_paired(), which is set optimistically before encryption
 * completes on reconnect.
 *
 * @return true if the active link is encrypted, false otherwise
 */
bool halo_ble_sec_is_encrypted(void);

/**
 * @brief Check if new pairing is allowed
 * 
 * Checks if a new device can pair based on current bond status
 * and CONFIG_HALO_BLE_ALLOW_BOND_OVERWRITE configuration.
 * 
 * @return true if pairing is allowed, false if bond exists and overwrite is disabled
 */
bool halo_ble_sec_can_pair(void);

/**
 * @brief Handle pairing success
 * 
 * @param conidx Connection index
 * @param metainfo Meta information
 * @param pairing_level Pairing level
 * @param enc_key_present Encryption key present
 * @param key_type Key type
 */
void halo_ble_sec_on_paired(uint8_t conidx, uint32_t metainfo, uint8_t pairing_level, bool enc_key_present, uint8_t key_type);

/**
 * @brief Handle pairing failure
 * 
 * @param conidx Connection index
 * @param reason Failure reason
 */
void halo_ble_sec_on_pair_failed(uint8_t conidx, uint16_t reason);

/**
 * @brief Handle encryption established
 *
 * Called when the controller reports the link is encrypted (auth_info with
 * encrypted=true), for both fresh pairing and bonded reconnection. Marks the
 * link encrypted and emits HALO_BLE_EVENT_ENCRYPTED.
 *
 * @param conidx Connection index
 * @param sec_lvl Link security level (see gap_sec_lvl)
 * @param key_size Encryption key size
 */
void halo_ble_sec_on_encrypted(uint8_t conidx, uint8_t sec_lvl, uint8_t key_size);

/**
 * @brief Handle encryption failure
 * 
 * Called when link encryption fails, typically when peer device
 * has deleted bond data but local device still has stale keys.
 * 
 * @param conidx Connection index
 * @param reason Failure reason
 */
void halo_ble_sec_on_encrypt_failed(uint8_t conidx, uint16_t reason);

/**
 * @brief Handle encryption request
 * 
 * @param conidx Connection index
 * @return 0 on success, negative error code on failure
 */
int halo_ble_sec_on_encrypt_req(uint8_t conidx);

/**
 * @brief Handle received pairing keys
 * 
 * @param conidx Connection index
 * @param p_keys Received keys
 */
void halo_ble_sec_on_keys_received(uint8_t conidx, const gapc_pairing_keys_t *p_keys);

/**
 * @brief Provide IRK to peer device
 * 
 * @param conidx Connection index
 * @return 0 on success, negative error code on failure
 */
int halo_ble_sec_provide_irk(uint8_t conidx);

/**
 * @brief Provide CSRK to peer device
 * 
 * @param conidx Connection index
 * @return 0 on success, negative error code on failure
 */
int halo_ble_sec_provide_csrk(uint8_t conidx);

/**
 * @brief Provide LTK to peer device
 * 
 * @param conidx Connection index
 * @param key_size Key size
 * @return 0 on success, negative error code on failure
 */
int halo_ble_sec_provide_ltk(uint8_t conidx, uint8_t key_size);

/**
 * @brief Handle connection request with security check
 * 
 * This function checks if the peer device is bonded and handles
 * connection confirmation and pairing accordingly.
 * 
 * @param conidx Connection index
 * @param p_peer_addr Peer device address
 * @return 0 on success, negative error code on failure
 */
int halo_ble_sec_on_connection_req(uint8_t conidx, const gap_bdaddr_t *p_peer_addr);

#ifdef __cplusplus
}
#endif

#endif /* HALO_BLE_SECURITY_H */
