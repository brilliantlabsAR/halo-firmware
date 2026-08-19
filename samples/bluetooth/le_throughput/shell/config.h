/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "gap.h"

enum scan_type {
	SCAN_TYPE_GENERIC,
	SCAN_TYPE_CONNECTABLE,
	SCAN_TYPE_LIMITED,
};

enum scan_properties {
	SCAN_PROP_PHY_1M,
	SCAN_PROP_PHY_CODED,
	SCAN_PROP_ACTIVE_1M,
	SCAN_PROP_ACTIVE_CODED,
	SCAN_PROP_FILT_TRUNK,
};

struct scan_config {
	uint16_t interval;
	uint16_t window;
	/* enum scan_type */
	uint8_t type;
	/* enum scan_properties */
	uint8_t properties;
	uint8_t filter;
};

int scan_create_and_start(struct scan_config const *const p_config);
int scan_create_and_start_default(void);
int scan_stop(bool const sync);

int get_private_address(char *p_str, size_t len);
int convert_uuid_to_string(char *p_str, size_t max_len, uint8_t const *p_uuid, uint8_t uuid_type);
int convert_uuid_with_len_to_string(char *p_str, size_t max_len, uint8_t const *p_uuid,
				    size_t uuid_len);
void set_device_role(enum gap_role role);
enum gap_role get_device_role(void);
