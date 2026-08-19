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

struct tp_data {
	uint32_t write_count;
	uint32_t write_len;
	uint32_t write_rate;
};

enum app_state {
	APP_STATE_ERROR = -1,
	APP_STATE_STANDBY = 0,
	APP_STATE_INIT,
	APP_STATE_SCAN_START,
	APP_STATE_SCAN_ONGOING,
	APP_STATE_SCAN_READY,
	APP_STATE_PERIPHERAL_FOUND,
	APP_STATE_CONNECTING,
	APP_STATE_CONNECTED,
	APP_STATE_CONNECTED_PAIRED,
	APP_STATE_GET_FEATURES,
	APP_STATE_DISCOVER_SERVICES,
	APP_STATE_CENTRAL_READY,
	APP_STATE_DATA_TRANSMIT,
	APP_STATE_DATA_READ,
	APP_STATE_DATA_SEND_READY,
	APP_STATE_DATA_RECEIVE_READY,
	APP_STATE_STATS_RESET,
	APP_STATE_PERIPHERAL_START_ADVERTISING,
	APP_STATE_PERIPHERAL_RECEIVING,
	APP_STATE_PERIPHERAL_PREPARE_SENDING,
	APP_STATE_PERIPHERAL_SENDING,
	APP_STATE_PERIPHERAL_SEND_RESULTS,
	APP_STATE_DISCONNECT,
	APP_STATE_DISCONNECTED,
};

enum tp_client_ctrl_type {
	TP_CLIENT_CTRL_TYPE_RESET = 1,
};

struct tp_client_ctrl {
	enum tp_client_ctrl_type type;
	uint32_t test_duration_ms;
	uint32_t send_interval_ms;
};

void app_transition_to(enum app_state state);
enum app_state get_app_state(void);
uint8_t get_connection_index(void);
uint16_t get_mtu_size(void);
void tp_worker(void *p1, void *p2, void *p3);
