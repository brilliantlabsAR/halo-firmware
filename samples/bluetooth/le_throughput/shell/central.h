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

struct central_conn_params {
	uint32_t conn_interval_min;
	uint32_t conn_interval_max;
	uint32_t supervision_to;
};

/**
 * Init BLE central functionality
 */
void central_app_init(void);

/**
 * App central state machine execution
 *
 * @param[in] state Application state
 *
 * @return 0 in case of state was handled, otherwise -1
 */
int central_app_exec(uint32_t const state);

/**
 * Get service UUID string
 *
 * @param[out] p_uuid Service UUID string
 * @param[in] max_len Maximum length of UUID string
 *
 * @return 0 in case of success, otherwise -1
 */
int central_get_service_uuid_str(char *p_uuid, uint8_t max_len);

/**
 * Set send interval
 *
 * @param[in] interval Send interval in ms
 *
 * @return 0 in case of success, otherwise -1
 */
int central_set_send_interval(uint32_t interval);

/**
 * Get connection parameters
 *
 * @param[out] p_params Connection parameters
 *
 * @return 0 in case of success, otherwise -1
 */
int central_connection_params_get(struct central_conn_params *p_params);

/**
 * Set connection parameters
 *
 * @param[in] p_params Connection parameters
 *
 * @return 0 in case of success, otherwise -1
 */
int central_connection_params_set(struct central_conn_params const *p_params);

/**
 * Set test duration
 *
 * @param[in] duration Test duration in seconds
 *
 * @return 0 in case of success, otherwise -1
 */
int central_set_test_duration(uint32_t duration_s);
