/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_BLE_OTA_H
#define HALO_BLE_OTA_H

#include <stdint.h>
#include <stddef.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the OTA service
 *
 * This service implements SMP (Simple Management Protocol) over BLE
 * for firmware updates and device management.
 *
 * Service UUID: 8D53DC1D-1DB7-4CD3-868B-8A527460AA84
 * Characteristic UUID: DA2E7828-FBCE-4E01-AE9E-261174997C48
 *
 * @param reset If true, reset the service state
 * @return 0 on success, negative error code on failure
 */
int halo_ble_ota_init(bool reset);

/**
 * @brief Deinitialize the OTA service
 *
 * @return 0 on success, negative error code on failure
 */
int halo_ble_ota_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* HALO_BLE_OTA_H */
