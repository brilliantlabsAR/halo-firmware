/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_BLE_SERVICE_H
#define HALO_BLE_SERVICE_H

#include <zephyr/kernel.h>
#include <zephyr/sys/slist.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct halo_ble_service {
sys_snode_t node;
const char *name;

int (*init)(void);
int (*deinit)(void);
int (*on_connected)(uint8_t conidx);
int (*on_disconnected)(uint8_t conidx);
int (*on_paired)(uint8_t conidx);

uint16_t start_hdl;
bool registered;
void *user_data;
};

int halo_ble_service_init(void);
int halo_ble_service_register(struct halo_ble_service *svc);
int halo_ble_service_unregister(struct halo_ble_service *svc);
struct halo_ble_service *halo_ble_service_find_by_name(const char *name);
bool halo_ble_service_is_registered(const struct halo_ble_service *service);
void halo_ble_service_on_connected(uint8_t conidx);
void halo_ble_service_on_disconnected(uint8_t conidx);
void halo_ble_service_on_paired(uint8_t conidx);

#ifdef __cplusplus
}
#endif

#endif
