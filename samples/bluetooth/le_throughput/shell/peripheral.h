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

struct peripheral_conn_params {
	uint32_t conn_interval_min;
	uint32_t conn_interval_max;
	uint32_t supervision_to;
};

void peripheral_app_init(void);

int peripheral_app_exec(uint32_t const state);

/**
 * Get peripheral serive UUID
 *
 * @return 0 in case of success, negative value is case of error
 */
int peripheral_get_service_uuid_str(char *p_uuid, uint8_t max_len);

/**
 * Set connection parameters
 *
 * @param[in] p_params Connection parameters
 *
 * @return 0 in case of success, otherwise -1
 */
int peripheral_connection_params_set(struct peripheral_conn_params const *p_params);
