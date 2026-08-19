/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#pragma once

#include "gatt.h"

struct conn_handle {
	uint16_t handle;
	uint8_t conidx;
};

struct conn_uuid {
	uint8_t *p_uuid;
	uint8_t uuid_type;
	uint8_t conidx;
};

int gatt_client_register(void);
int gatt_client_discover_primary_all(uint8_t conidx);
int gatt_client_discover_primary_by_uuid(struct conn_uuid uuid, uint16_t *p_handle_found);
int gatt_client_read(struct conn_handle handle, size_t size);
int gatt_client_read_by_uuid(struct conn_uuid uuid, size_t size);
int gatt_client_write_ack(struct conn_handle handle, char const *p_data, size_t size);
int gatt_client_write_noack(struct conn_handle handle, char const *p_data, size_t size);
int gatt_client_register_event(struct conn_handle const handle);
