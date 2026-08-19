/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_BLE_INTERNAL_H
#define HALO_BLE_INTERNAL_H

#include "halo/ble_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Notify BLE event (internal use only)
 * 
 * This function is called by sub-modules to dispatch events to registered callbacks.
 * 
 * @param event Event data
 */
void halo_ble_notify_event(const struct halo_ble_event_data *event);

#ifdef __cplusplus
}
#endif

#endif /* HALO_BLE_INTERNAL_H */
