/*
 * Copyright (c) 2024 Alif Semiconductor
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief BLE Throughput Test for Halo Board
 *
 * This module implements a BLE UART service for testing read/write throughput.
 * Features:
 * - BLE GATT server with custom UART service (Nordic UART Service compatible)
 * - Notification-based data transmission (TX)
 * - Write characteristic for data reception (RX)
 * - Real-time throughput measurement and reporting
 * - Automatic reconnection on disconnection
 *
 * Service UUIDs:
 * - Service: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 * - TX (Notify): 6E400003-B5A3-F393-E0A9-E50E24DCCA9E
 * - RX (Write): 6E400002-B5A3-F393-E0A9-E50E24DCCA9E
 *
 * Usage:
 *   uart:~$ ble init
 *   # Device will advertise and accept connections
 *   # Enable notifications from central device
 *   # Throughput stats printed every 1 second
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if CONFIG_BT_CUSTOM

#include <zephyr/drivers/gpio.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>

#include "alif_ble.h"
#include "gapm.h"
#include "gap_le.h"
#include "gapc_le.h"
#include "gapc_sec.h"
#include "gapm_le.h"
#include "gapm_le_adv.h"
#include "co_buf.h"
#include "prf.h"
#include "gatt_db.h"
#include "gatt_srv.h"
#include "ke_mem.h"

LOG_MODULE_REGISTER(ble_throughput, LOG_LEVEL_INF);

/* Device name for advertising */
#define DEVICE_NAME CONFIG_BLE_DEVICE_NAME
static const char device_name[] = DEVICE_NAME;

/* BLE task thread configuration */
#define BLE_THREAD_STACK_SIZE 1024
#define BLE_THREAD_PRIORITY   K_PRIO_COOP(7)

K_THREAD_STACK_DEFINE(ble_thread_stack, BLE_THREAD_STACK_SIZE);
static k_tid_t tid;
static struct k_thread ble_thread;

/* Synchronization primitives */
K_SEM_DEFINE(init_sem, 0, 1);

/* Connection state tracking */
#define BT_CONN_STATE_CONNECTED    0x00
#define BT_CONN_STATE_DISCONNECTED 0x01

/**
 * @brief Nordic UART Service (NUS) compatible UUIDs
 *
 * These UUIDs are compatible with nRF Connect and other Nordic UART Service
 * based applications.
 *
 * Service UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 * TX UUID:      6E400003-B5A3-F393-E0A9-E50E24DCCA9E (Notify)
 * RX UUID:      6E400002-B5A3-F393-E0A9-E50E24DCCA9E (Write)
 */
#define UART_UUID_128_SVC                                                                          \
	{0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,                                           \
	 0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E}
#define UART_UUID_128_TX                                                                           \
	{0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,                                           \
	 0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E}
#define UART_UUID_128_RX                                                                           \
	{0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,                                           \
	 0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E}

/* GATT notification metadata identifier */
#define METAINFO_TX_NTF_SEND 0x1234

/* Throughput test configuration */
#define TX_PACKET_SIZE       517  /* Bytes per transmission */
#define TX_INTERVAL_MS       1  /* Milliseconds between transmissions */
#define STATS_INTERVAL_MS    1000 /* Statistics reporting interval */

/* Helper macros for UUID conversion */
#define ATT_16_TO_128_ARRAY(uuid)                                                                  \
	{(uuid) & 0xFF, (uuid >> 8) & 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}

#define ATT_128_PRIMARY_SERVICE  ATT_16_TO_128_ARRAY(GATT_DECL_PRIMARY_SERVICE)
#define ATT_128_INCLUDED_SERVICE ATT_16_TO_128_ARRAY(GATT_DECL_INCLUDE)
#define ATT_128_CHARACTERISTIC   ATT_16_TO_128_ARRAY(GATT_DECL_CHARACTERISTIC)
#define ATT_128_CLIENT_CHAR_CFG  ATT_16_TO_128_ARRAY(GATT_DESC_CLIENT_CHAR_CFG)

/**
 * @brief UART Service Attribute Indices
 *
 * Defines the attribute handle indices within the UART GATT service.
 */
enum uart_att_list {
	UART_IDX_SERVICE = 0,  /* Primary service declaration */
	UART_IDX_TX_CHAR,      /* TX characteristic declaration */
	UART_IDX_TX_VAL,       /* TX characteristic value (notify) */
	UART_IDX_TX_NTF_CFG,   /* TX CCCD (notification configuration) */
	UART_IDX_RX_CHAR,      /* RX characteristic declaration */
	UART_IDX_RX_VAL,       /* RX characteristic value (write) */
	UART_IDX_NB,           /* Total number of attributes */
};

/* UART service UUID */
static const uint8_t uart_service_uuid[] = UART_UUID_128_SVC;

/**
 * @brief UART Service GATT Attribute Database
 *
 * Defines the complete attribute structure for the UART service:
 * - Primary Service (16-bit UUID)
 * - TX Characteristic with Notification support (128-bit UUID)
 * - TX Client Characteristic Configuration Descriptor (CCCD)
 * - RX Characteristic with Write support (128-bit UUID, max 512 bytes)
 */
static const gatt_att_desc_t uart_att_db[UART_IDX_NB] = {
	[UART_IDX_SERVICE] = {ATT_128_PRIMARY_SERVICE, ATT_UUID(16) | PROP(RD), 0},
	[UART_IDX_TX_CHAR] = {ATT_128_CHARACTERISTIC, ATT_UUID(16) | PROP(RD), 0},
	[UART_IDX_TX_VAL] = {UART_UUID_128_TX, ATT_UUID(128) | PROP(RD) | PROP(N), OPT(NO_OFFSET)},
	[UART_IDX_TX_NTF_CFG] = {ATT_128_CLIENT_CHAR_CFG, ATT_UUID(16) | PROP(RD) | PROP(WR), 0},
	[UART_IDX_RX_CHAR] = {ATT_128_CHARACTERISTIC, ATT_UUID(16) | PROP(RD), 0},
	[UART_IDX_RX_VAL] = {UART_UUID_128_RX, ATT_UUID(128) | PROP(WR), OPT(NO_OFFSET) | TX_PACKET_SIZE},
};

/**
 * @brief UART Service Runtime Environment
 *
 * Maintains the state and buffers for the UART service including:
 * - GATT handles and user LID
 * - TX/RX data buffers
 * - Notification control and synchronization
 */
struct uart_env {
	uint16_t start_hdl;                /* Service start handle */
	uint8_t user_lid;                  /* GATT user local identifier */
	uint8_t tx_data[TX_PACKET_SIZE];   /* TX buffer for notifications */
	uint8_t rx_data[TX_PACKET_SIZE];   /* RX buffer for writes */
	uint16_t rx_data_len;              /* Received data length */
	struct k_sem ntf;                  /* Notification semaphore */
	uint16_t ntf_cfg;                  /* CCCD configuration */
	uint32_t tx_bytes;                 /* Total TX bytes (for stats) */
	uint32_t rx_bytes;                 /* Total RX bytes (for stats) */
};

/* Global state variables */
static struct uart_env uart_env;
static uint8_t conn_status = BT_CONN_STATE_DISCONNECTED;
static uint8_t adv_actv_idx;

static gapm_config_t gapm_cfg = {
	.role = GAP_ROLE_LE_PERIPHERAL,
	/* -------------- Security Config ------------------------------------ */
	.pairing_mode = GAPM_PAIRING_DISABLE,

	/* -------------- Privacy Config ------------------------------------- */
	.privacy_cfg = GAPM_PRIV_CFG_PRIV_ADDR_BIT,
	.renew_dur = 1500,
	.private_identity =
		{
			.addr = {0xCF, 0xFE, 0xFB, 0xDE, 0x11, 0x08},
		},

	.irk.key = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, /* ignored */

	/* -------------- ATT Database Config -------------------------------- */
	.gap_start_hdl = 0,
	.gatt_start_hdl = 0,
	.att_cfg = 0,

	/* -------------- LE Data Length Extension --------------------------- */
	.sugg_max_tx_octets = GAP_LE_MAX_OCTETS,
	.sugg_max_tx_time = GAP_LE_MAX_TIME,

	/* ------------------ LE PHY Management  ----------------------------- */
	.tx_pref_phy = GAP_PHY_LE_2MBPS,
	.rx_pref_phy = GAP_PHY_LE_2MBPS,

	/* ------------------ Radio Configuration ---------------------------- */
	.tx_path_comp = 0,
	.rx_path_comp = 0,

	/* ------------------ BT classic configuration ---------------------- */
	/* Not used */
	.class_of_device = 0,
	.dflt_link_policy = 0,
};

/**
 * @brief GATT Attribute Read Callback
 *
 * Handles read requests for TX characteristic value and CCCD.
 *
 * @param conidx Connection index
 * @param user_lid User local identifier
 * @param token Request token
 * @param hdl Attribute handle being read
 * @param offset Read offset
 * @param max_length Maximum read length
 */
static void on_att_read_get(uint8_t conidx, uint8_t user_lid, uint16_t token, uint16_t hdl,
			    uint16_t offset, uint16_t max_length)
{
	co_buf_t *p_buf = NULL;
	uint16_t status = GAP_ERR_NO_ERROR;
	uint16_t att_val_len = 0;
	void *att_val = NULL;

	uint8_t att_idx = hdl - uart_env.start_hdl;

	switch (att_idx) {
	case UART_IDX_TX_VAL:
		att_val_len = sizeof(uart_env.tx_data);
		att_val = uart_env.tx_data;
		LOG_DBG("Read TX value, length %u", att_val_len);
		break;
	case UART_IDX_TX_NTF_CFG:
		att_val_len = sizeof(uart_env.ntf_cfg);
		att_val = &uart_env.ntf_cfg;
		LOG_DBG("Read CCCD, value 0x%04X", uart_env.ntf_cfg);
		break;
	default:
		LOG_WRN("Read request for unsupported attribute %u", att_idx);
		status = ATT_ERR_REQUEST_NOT_SUPPORTED;
		break;
	}

	if (status == GAP_ERR_NO_ERROR) {
		co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, att_val_len, GATT_BUFFER_TAIL_LEN);
		memcpy(co_buf_data(p_buf), att_val, att_val_len);
	}

	gatt_srv_att_read_get_cfm(conidx, user_lid, token, status, att_val_len, p_buf);
	if (p_buf) {
		co_buf_release(p_buf);
	}
}

/**
 * @brief GATT Attribute Write Callback
 *
 * Handles write requests for RX characteristic and CCCD configuration.
 * Tracks received bytes for throughput statistics.
 *
 * @param conidx Connection index
 * @param user_lid User local identifier
 * @param token Request token
 * @param hdl Attribute handle being written
 * @param offset Write offset
 * @param p_data Data buffer to write
 */
static void on_att_val_set(uint8_t conidx, uint8_t user_lid, uint16_t token, uint16_t hdl,
			   uint16_t offset, co_buf_t *p_data)
{
	uint16_t status = GAP_ERR_NO_ERROR;
	uint8_t att_idx = hdl - uart_env.start_hdl;

	switch (att_idx) {
	case UART_IDX_RX_VAL: {
		uint16_t data_len = co_buf_data_len(p_data);
		if (data_len > sizeof(uart_env.rx_data)) {
			LOG_ERR("RX data too large: %u bytes (max %u)", data_len, sizeof(uart_env.rx_data));
			status = ATT_ERR_INVALID_ATTRIBUTE_VAL_LEN;
		} else {
			memcpy(uart_env.rx_data, co_buf_data(p_data), data_len);
			uart_env.rx_data_len = data_len;
			uart_env.rx_bytes += data_len;
			LOG_DBG("Received %u bytes (total: %u)", data_len, uart_env.rx_bytes);
		}
		break;
	}
	case UART_IDX_TX_NTF_CFG: {
		uint16_t cfg;
		memcpy(&cfg, co_buf_data(p_data), sizeof(cfg));
		uart_env.ntf_cfg = (cfg == PRF_CLI_START_NTF) ? cfg : PRF_CLI_STOP_NTFIND;
		
		if (uart_env.ntf_cfg == PRF_CLI_START_NTF) {
			printk("Notifications ENABLED - Starting throughput test\n");
			LOG_INF("TX notifications enabled");
		} else {
			printk("Notifications DISABLED - Stopping throughput test\n");
			LOG_INF("TX notifications disabled");
		}
		break;
	}
	default:
		LOG_WRN("Write request for unsupported attribute %u", att_idx);
		status = ATT_ERR_REQUEST_NOT_SUPPORTED;
		break;
	}

	gatt_srv_att_val_set_cfm(conidx, user_lid, token, status);
}

/**
 * @brief GATT Event Sent Callback
 *
 * Called when a notification has been sent. Releases the semaphore
 * to allow the next notification to be sent.
 *
 * @param conidx Connection index
 * @param user_lid User local identifier
 * @param metainfo Metadata identifier
 * @param status Operation status
 */
static void on_event_sent(uint8_t conidx, uint8_t user_lid, uint16_t metainfo, uint16_t status)
{
	if (metainfo == METAINFO_TX_NTF_SEND) {
		if (status != GAP_ERR_NO_ERROR) {
			LOG_WRN("Notification failed with status %u", status);
		}
		k_sem_give(&uart_env.ntf);
	}
}

/**
 * @brief GATT Service Callbacks
 *
 * Defines the callback functions for GATT attribute operations.
 */
static const gatt_srv_cb_t gatt_cbs = {
	.cb_att_event_get = NULL,
	.cb_att_info_get = NULL,
	.cb_att_read_get = on_att_read_get,
	.cb_att_val_set = on_att_val_set,
	.cb_event_sent = on_event_sent,
};

/**
 * @brief Initialize UART GATT Service
 *
 * Registers the UART service with the GATT database.
 *
 * @return GAP_ERR_NO_ERROR on success, error code otherwise
 */
static uint16_t uart_service_init(void)
{
	uint16_t status;
	
	/* Register GATT user service with MTU size of 517 bytes */
	status = gatt_user_srv_register(517, 0, &gatt_cbs, &uart_env.user_lid);
	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to register GATT user service: %u", status);
		return status;
	}

	/* Add service to GATT database */
	status = gatt_db_svc_add(uart_env.user_lid, SVC_UUID(128), uart_service_uuid, UART_IDX_NB,
			       NULL, uart_att_db, UART_IDX_NB, &uart_env.start_hdl);
	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to add UART service to database: %u", status);
		return status;
	}

	LOG_INF("UART service initialized, start handle: 0x%04X", uart_env.start_hdl);
	return status;
}

/**
 * @brief Send Data via UART TX Notification
 *
 * Sends data to the connected central device via BLE notification.
 * This function is thread-safe and blocks until the previous notification
 * is completed.
 *
 * @param data Pointer to data buffer
 * @param len Length of data to send (max TX_PACKET_SIZE)
 * @return GAP_ERR_NO_ERROR on success, error code otherwise
 */
static uint16_t uart_send_data(const uint8_t *data, uint16_t len)
{
	co_buf_t *p_buf;
	uint16_t status;
	uint8_t conidx = 0;

	/* Check if notifications are enabled */
	if (uart_env.ntf_cfg != PRF_CLI_START_NTF) {
		return PRF_ERR_NTF_DISABLED;
	}

	/* Wait for previous notification to complete */
	k_sem_take(&uart_env.ntf, K_FOREVER);

	alif_ble_mutex_lock(K_FOREVER);

	/* Allocate buffer for notification */
	status = co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, len, GATT_BUFFER_TAIL_LEN);
	if (status != CO_BUF_ERR_NO_ERROR) {
		LOG_ERR("Failed to allocate buffer: %u", status);
		alif_ble_mutex_unlock();
		k_sem_give(&uart_env.ntf);
		return GAP_ERR_INSUFF_RESOURCES;
	}

	/* Copy data to buffer */
	memcpy(co_buf_data(p_buf), data, len);

	/* Send notification */
	status = gatt_srv_event_send(conidx, uart_env.user_lid, METAINFO_TX_NTF_SEND,
				     GATT_NOTIFY, uart_env.start_hdl + UART_IDX_TX_VAL, p_buf);

	co_buf_release(p_buf);
	
	if (status == GAP_ERR_NO_ERROR) {
		uart_env.tx_bytes += len;
	} else {
		LOG_ERR("Failed to send notification: %u", status);
		k_sem_give(&uart_env.ntf);
	}
	
	alif_ble_mutex_unlock();

	return status;
}

/* Functions */
static uint16_t start_le_adv(uint8_t actv_idx)
{
	uint16_t err;
	gapm_le_adv_param_t adv_params = {
		/* Advertise indefinitely */
		.duration = 0,
	};

	err = gapm_le_start_adv(actv_idx, &adv_params);
	if (err) {
		LOG_ERR("Failed to start LE advertising with error %u", err);
	}
	return err;
}

/**
 * @brief Handle BLE Connection Request
 *
 * Called when a central device requests to connect. This function:
 * 1. Confirms the connection
 * 2. Initializes connection state
 * 3. Resets notification configuration (client must re-enable)
 * 4. Prepares for data transmission
 *
 * @param conidx Connection index
 * @param metainfo Metadata information
 * @param actv_idx Activity index
 * @param role Connection role (peripheral)
 * @param p_peer_addr Peer device address
 * @param p_con_params Connection parameters
 * @param clk_accuracy Clock accuracy
 */
static void on_le_connection_req(uint8_t conidx, uint32_t metainfo, uint8_t actv_idx, uint8_t role,
				 const gap_bdaddr_t *p_peer_addr,
				 const gapc_le_con_param_t *p_con_params, uint8_t clk_accuracy)
{
	printk("\n=== BLE Connected ===\n");
	printk("Connection index: %u\n", conidx);
	printk("Peer address: %02X:%02X:%02X:%02X:%02X:%02X\n",
	       p_peer_addr->addr[5], p_peer_addr->addr[4], p_peer_addr->addr[3],
	       p_peer_addr->addr[2], p_peer_addr->addr[1], p_peer_addr->addr[0]);
	printk("Connection interval: %.2f ms\n", (double)p_con_params->interval * 1.25);
	printk("Latency: %u\n", p_con_params->latency);
	printk("Supervision timeout: %u ms\n", p_con_params->sup_to * 10);
	printk("Enable notifications to start throughput test\n\n");

	LOG_INF("Connection request on index %u", conidx);
	LOG_DBG("Connection parameters: interval %u, latency %u, supervision timeout %u",
		p_con_params->interval, p_con_params->latency, p_con_params->sup_to);
	LOG_INF("Peer BD address %02X:%02X:%02X:%02X:%02X:%02X (conidx: %u)", 
		p_peer_addr->addr[5], p_peer_addr->addr[4], p_peer_addr->addr[3], 
		p_peer_addr->addr[2], p_peer_addr->addr[1], p_peer_addr->addr[0], conidx);

	/* Confirm connection */
	gapc_le_connection_cfm(conidx, 0, NULL);

	/* Initialize connection state */
	conn_status = BT_CONN_STATE_CONNECTED;
	
	/* Reset notification state - client must re-enable after connection */
	uart_env.ntf_cfg = PRF_CLI_STOP_NTFIND;
	
	/* Clear any stale data from previous connection */
	memset(uart_env.tx_data, 0, sizeof(uart_env.tx_data));
	memset(uart_env.rx_data, 0, sizeof(uart_env.rx_data));
	uart_env.rx_data_len = 0;
	
	/* Reset notification semaphore */
	k_sem_reset(&uart_env.ntf);
	k_sem_give(&uart_env.ntf);
}

static void on_key_received(uint8_t conidx, uint32_t metainfo, const gapc_pairing_keys_t *p_keys)
{
	LOG_WRN("Unexpected key received key on conidx %u", conidx);
}

/**
 * @brief Handle BLE Disconnection Event
 *
 * Called when the BLE connection is terminated. This function:
 * 1. Resets notification state
 * 2. Clears any pending data buffers
 * 3. Updates connection status
 * 4. Restarts advertising for reconnection
 *
 * @param conidx Connection index that was disconnected
 * @param metainfo Metadata information
 * @param reason Disconnection reason code
 */
static void on_disconnection(uint8_t conidx, uint32_t metainfo, uint16_t reason)
{
	uint16_t err;

	printk("\n=== BLE Disconnected ===\n");
	printk("Connection index: %u\n", conidx);
	printk("Reason: 0x%04X\n", reason);
	LOG_INF("Connection index %u disconnected for reason 0x%04X", conidx, reason);

	/* Reset notification state */
	uart_env.ntf_cfg = PRF_CLI_STOP_NTFIND;
	
	/* Reset notification semaphore to initial state */
	k_sem_reset(&uart_env.ntf);
	k_sem_give(&uart_env.ntf);
	
	/* Clear data buffers */
	memset(uart_env.tx_data, 0, sizeof(uart_env.tx_data));
	memset(uart_env.rx_data, 0, sizeof(uart_env.rx_data));
	uart_env.rx_data_len = 0;
	
	/* Note: Keep tx_bytes and rx_bytes for statistics across connections */
	/* To reset them, user can use "ble reset" command */
	
	/* Update connection status */
	conn_status = BT_CONN_STATE_DISCONNECTED;

	/* Restart advertising to allow reconnection */
	err = start_le_adv(adv_actv_idx);
	if (err) {
		printk("Error restarting advertising: %u\n", err);
		LOG_ERR("Error restarting advertising: %u", err);
	} else {
		printk("Advertising restarted - waiting for connection...\n\n");
		LOG_INF("Advertising restarted successfully");
	}
}

static void on_name_get(uint8_t conidx, uint32_t metainfo, uint16_t token, uint16_t offset,
			uint16_t max_len)
{
	const size_t device_name_len = sizeof(device_name) - 1;
	const size_t short_len = (device_name_len > max_len ? max_len : device_name_len);

	gapc_le_get_name_cfm(conidx, token, GAP_ERR_NO_ERROR, device_name_len, short_len,
			     (const uint8_t *)device_name);
}

static void on_appearance_get(uint8_t conidx, uint32_t metainfo, uint16_t token)
{
	/* Send 'unknown' appearance */
	gapc_le_get_appearance_cfm(conidx, token, GAP_ERR_NO_ERROR, 0);
}

static const gapc_connection_req_cb_t gapc_con_cbs = {
	.le_connection_req = on_le_connection_req,
};

static const gapc_security_cb_t gapc_sec_cbs = {
	.key_received = on_key_received,
	/* All other callbacks in this struct are optional */
};

static const gapc_connection_info_cb_t gapc_con_inf_cbs = {
	.disconnected = on_disconnection,
	.name_get = on_name_get,
	.appearance_get = on_appearance_get,
	/* Other callbacks in this struct are optional */
};

/* All callbacks in this struct are optional */
static const gapc_le_config_cb_t gapc_le_cfg_cbs;

#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
static void on_gapm_err(uint32_t metainfo, uint8_t code)
{
	LOG_ERR("gapm error %d", code);
}

static const gapm_cb_t gapm_err_cbs = {
	.cb_hw_error = on_gapm_err,
};

static const gapm_callbacks_t gapm_cbs = {
	.p_con_req_cbs = &gapc_con_cbs,
	.p_sec_cbs = &gapc_sec_cbs,
	.p_info_cbs = &gapc_con_inf_cbs,
	.p_le_config_cbs = &gapc_le_cfg_cbs,
	.p_bt_config_cbs = NULL, /* BT classic so not required */
	.p_gapm_cbs = &gapm_err_cbs,
};
#else /* ROM version 1.0 */
static void on_gapm_err(enum co_error err)
{
	LOG_ERR("gapm error %d", err);
}

static const gapm_err_info_config_cb_t gapm_err_cbs = {
	.ctrl_hw_error = on_gapm_err,
};

static const gapm_callbacks_t gapm_cbs = {
	.p_con_req_cbs = &gapc_con_cbs,
	.p_sec_cbs = &gapc_sec_cbs,
	.p_info_cbs = &gapc_con_inf_cbs,
	.p_le_config_cbs = &gapc_le_cfg_cbs,
	.p_bt_config_cbs = NULL, /* BT classic so not required */
	.p_err_info_config_cbs = &gapm_err_cbs,
};
#endif

static void server_configure(void)
{

	/* Initialize GAPM */
	if (uart_service_init() != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to initialize UART service");
		return;
	}

	return;
}

static uint16_t set_advertising_data(uint8_t actv_idx)
{
	uint16_t err;

	/* gatt service identifier */
	uint8_t svc[16] = UART_UUID_128_SVC;

	/* Name advertising length */
	const size_t device_name_len = sizeof(device_name) - 1;
	const uint16_t adv_device_name = GATT_HANDLE_LEN + device_name_len;

	/* Service advertising length */
	const uint16_t adv_uuid_svc = GATT_HANDLE_LEN + GATT_UUID_128_LEN;

	/* Create advertising data with necessary services */
	const uint16_t adv_len = adv_device_name + adv_uuid_svc;

	co_buf_t *p_buf;
	uint8_t *p_data;

	err = co_buf_alloc(&p_buf, 0, adv_len, 0);
	if (err != 0) {
		LOG_ERR("Buffer allocation failed");
		return err;
	}

	p_data = co_buf_data(p_buf);

	/* Device name data */
	p_data[0] = device_name_len + 1;
	p_data[1] = GAP_AD_TYPE_COMPLETE_NAME;
	memcpy(p_data + 2, device_name, device_name_len);

	/* Update data pointer */
	p_data = p_data + adv_device_name;

	/* Service UUID data */
	p_data[0] = GATT_UUID_128_LEN + 1;
	p_data[1] = GAP_AD_TYPE_COMPLETE_LIST_128_BIT_UUID;
	memcpy(p_data + 2, &svc, sizeof(svc));

	err = gapm_le_set_adv_data(actv_idx, p_buf);
	co_buf_release(p_buf);

	if (err) {
		LOG_ERR("Failed to set advertising data with error %u", err);
	}

	return err;
}

static uint16_t set_scan_data(uint8_t actv_idx)
{
	co_buf_t *p_buf;
	uint16_t err = co_buf_alloc(&p_buf, 0, 0, 0);

	__ASSERT(err == 0, "Buffer allocation failed");

	err = gapm_le_set_scan_response_data(actv_idx, p_buf);
	if (err) {
		LOG_ERR("Failed to set scan data with error %u", err);
	}

	return err;
}

/**
 * Advertising callbacks
 */
static void on_adv_actv_stopped(uint32_t metainfo, uint8_t actv_idx, uint16_t reason)
{
	LOG_DBG("Advertising activity index %u stopped for reason %u", actv_idx, reason);
}

static void on_adv_actv_proc_cmp(uint32_t metainfo, uint8_t proc_id, uint8_t actv_idx,
				 uint16_t status)
{
	if (status) {
		LOG_ERR("Advertising activity process completed with error %u", status);
		return;
	}

	switch (proc_id) {
	case GAPM_ACTV_CREATE_LE_ADV:
		LOG_DBG("Advertising activity is created");
		adv_actv_idx = actv_idx;
		set_advertising_data(actv_idx);
		break;

	case GAPM_ACTV_SET_ADV_DATA:
		LOG_DBG("Advertising data is set");
		set_scan_data(actv_idx);
		break;

	case GAPM_ACTV_SET_SCAN_RSP_DATA:
		LOG_DBG("Scan data is set");
		start_le_adv(actv_idx);
		break;

	case GAPM_ACTV_START:
		LOG_DBG("Advertising was started");
		k_sem_give(&init_sem);
		break;

	default:
		LOG_WRN("Unexpected GAPM activity complete, proc_id %u", proc_id);
		break;
	}
}

static void on_adv_created(uint32_t metainfo, uint8_t actv_idx, int8_t tx_pwr)
{
	LOG_DBG("Advertising activity created, index %u, selected tx power %d", actv_idx, tx_pwr);
}

static const gapm_le_adv_cb_actv_t le_adv_cbs = {
	.hdr.actv.stopped = on_adv_actv_stopped,
	.hdr.actv.proc_cmp = on_adv_actv_proc_cmp,
	.created = on_adv_created,
};

static uint16_t create_advertising(void)
{
	uint16_t err;

	gapm_le_adv_create_param_t adv_create_params = {
		.prop = GAPM_ADV_PROP_UNDIR_CONN_MASK,
		.disc_mode = GAPM_ADV_MODE_GEN_DISC,
#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
		.tx_pwr = 0,
#else
		.max_tx_pwr = 0,
#endif /* !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 */
		.filter_pol = GAPM_ADV_ALLOW_SCAN_ANY_CON_ANY,
		.prim_cfg =
			{
				.adv_intv_min = 100,
				.adv_intv_max = 300,
				.ch_map = ADV_ALL_CHNLS_EN,
				.phy = GAPM_PHY_TYPE_LE_1M,
			},
	};

	err = gapm_le_create_adv_legacy(0, GAPM_STATIC_ADDR, &adv_create_params, &le_adv_cbs);
	if (err) {
		LOG_ERR("Error %u creating advertising activity", err);
	}

	return err;
}

void on_gapm_process_complete(uint32_t metainfo, uint16_t status)
{
	if (status) {
		LOG_ERR("gapm process completed with error %u", status);
		return;
	}
	printf("GAPM process completed successfully\n");

	server_configure();

	LOG_DBG("gapm process completed successfully");

	create_advertising();
}

/**
 * @brief BLE Throughput Test Thread
 *
 * Main thread that:
 * 1. Initializes BLE stack and UART service
 * 2. Starts advertising
 * 3. Continuously sends test data when notifications are enabled
 * 4. Reports throughput statistics every second
 *
 * The test pattern uses incrementing byte values to verify data integrity
 * on the receiving end.
 *
 * @param p1 Unused
 * @param p2 Unused  
 * @param p3 Unused
 */
static void ble_task_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint8_t data[TX_PACKET_SIZE];
	uint8_t pattern = 0;

	uint32_t tx_bytes_last = 0;
	uint32_t rx_bytes_last = 0;
	int64_t stats_time = 0;
	uint16_t packet_count = 0;

	/* Initialize BLE stack */
	LOG_INF("Enabling BLE stack...");
	alif_ble_enable(NULL);
	
	/* Initialize notification semaphore */
	k_sem_init(&uart_env.ntf, 1, 1);
	
	/* Configure GAPM and wait for completion */
	LOG_INF("Configuring GAPM...");
	gapm_configure(0, &gapm_cfg, &gapm_cbs, on_gapm_process_complete);
	k_sem_take(&init_sem, K_FOREVER);
	
	LOG_INF("BLE initialized successfully");
	printk("\n=== BLE Throughput Test Ready ===\n");
	printk("Device Name: %s\n", device_name);
	printk("Connect and enable notifications to start test\n\n");

	stats_time = k_uptime_get();

	while (1) {
		/* Only send data when notifications are enabled */
		if (uart_env.ntf_cfg == PRF_CLI_START_NTF && conn_status == BT_CONN_STATE_CONNECTED) {
			/* Fill buffer with test pattern (incrementing byte) */
			memset(data, pattern++, sizeof(data));
			
			/* Send data packet */
			uint16_t status = uart_send_data(data, sizeof(data));
			if (status == GAP_ERR_NO_ERROR) {
				packet_count++;
			} else if (status != PRF_ERR_NTF_DISABLED) {
				LOG_WRN("TX failed: %u", status);
			}

			/* Calculate and display throughput statistics */
			int64_t now = k_uptime_get();
			int64_t elapsed = now - stats_time;
			
			if (elapsed >= STATS_INTERVAL_MS) {
				uint32_t tx_delta = uart_env.tx_bytes - tx_bytes_last;
				uint32_t rx_delta = uart_env.rx_bytes - rx_bytes_last;
				double elapsed_sec = (double)elapsed / 1000.0;
				
				/* Calculate TX throughput */
				double tx_bytes_per_sec = (double)tx_delta / elapsed_sec;
				double tx_kbps = (tx_bytes_per_sec * 8.0) / 1000.0;
				
				/* Calculate RX throughput */
				double rx_bytes_per_sec = (double)rx_delta / elapsed_sec;
				double rx_kbps = (rx_bytes_per_sec * 8.0) / 1000.0;
				
				printk("[%lld ms] TX: %u B/s (%.2f kbps) | RX: %u B/s (%.2f kbps) | Packets: %u\n",
				       now,
				       (uint32_t)tx_bytes_per_sec, tx_kbps,
				       (uint32_t)rx_bytes_per_sec, rx_kbps,
				       packet_count);
				
				/* Reset counters */
				tx_bytes_last = uart_env.tx_bytes;
				rx_bytes_last = uart_env.rx_bytes;
				stats_time = now;
				packet_count = 0;
			}
			
			/* Throttle transmission rate */
			//k_sleep(K_MSEC(TX_INTERVAL_MS));
		} else {
			/* Idle state - check status periodically */
			k_sleep(K_MSEC(500));
		}
	}
}

/**
 * @brief Shell Command: Initialize BLE Throughput Test
 *
 * Starts the BLE throughput test thread which:
 * - Initializes BLE stack
 * - Creates and starts advertising
 * - Begins throughput testing when connected
 *
 * Usage: ble init
 *
 * @param sh Shell instance
 * @param argc Argument count
 * @param argv Argument vector
 * @return 0 on success
 */
static int cmd_ble_init(const struct shell *sh, size_t argc, char *argv[])
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (tid != NULL) {
		shell_error(sh, "BLE thread already running");
		return -EALREADY;
	}

	shell_print(sh, "Initializing BLE throughput test...");
	shell_print(sh, "  - Device: %s", device_name);
	shell_print(sh, "  - Packet size: %u bytes", TX_PACKET_SIZE);
	shell_print(sh, "  - TX interval: %u ms", TX_INTERVAL_MS);
	
	/* Reset statistics */
	memset(&uart_env, 0, sizeof(uart_env));
	
	/* Create BLE task thread */
	tid = k_thread_create(&ble_thread, ble_thread_stack,
			      K_THREAD_STACK_SIZEOF(ble_thread_stack), 
			      ble_task_thread, NULL, NULL, NULL,
			      BLE_THREAD_PRIORITY, 0, K_NO_WAIT);
	
	if (tid == NULL) {
		shell_error(sh, "Failed to create BLE thread");
		return -ENOMEM;
	}
	
	k_thread_name_set(tid, "ble_throughput");
	
	shell_print(sh, "BLE thread started successfully");
	LOG_INF("BLE throughput test initialized");
	
	return 0;
}

/**
 * @brief Shell Command: Get BLE Statistics
 *
 * Displays current BLE throughput statistics.
 *
 * Usage: ble stats
 */
static int cmd_ble_stats(const struct shell *sh, size_t argc, char *argv[])
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "\n=== BLE Throughput Statistics ===");
	shell_print(sh, "Connection: %s", 
		    conn_status == BT_CONN_STATE_CONNECTED ? "CONNECTED" : "DISCONNECTED");
	shell_print(sh, "Notifications: %s", 
		    uart_env.ntf_cfg == PRF_CLI_START_NTF ? "ENABLED" : "DISABLED");
	shell_print(sh, "Total TX: %u bytes (%.2f KB)", 
		    uart_env.tx_bytes, (double)uart_env.tx_bytes / 1024.0);
	shell_print(sh, "Total RX: %u bytes (%.2f KB)", 
		    uart_env.rx_bytes, (double)uart_env.rx_bytes / 1024.0);

	return 0;
}

/**
 * @brief Shell Command: Reset BLE Statistics
 *
 * Resets throughput counters to zero.
 *
 * Usage: ble reset
 */
static int cmd_ble_reset(const struct shell *sh, size_t argc, char *argv[])
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	uart_env.tx_bytes = 0;
	uart_env.rx_bytes = 0;
	
	shell_print(sh, "Statistics reset");
	LOG_INF("BLE statistics reset");
	
	return 0;
}

/* BLE shell commands */
SHELL_STATIC_SUBCMD_SET_CREATE(sub_ble,
	SHELL_CMD_ARG(init, NULL, 
		"Initialize BLE throughput test\n"
		"Usage: ble init", 
		cmd_ble_init, 1, 0),
	SHELL_CMD_ARG(stats, NULL, 
		"Show throughput statistics\n"
		"Usage: ble stats", 
		cmd_ble_stats, 1, 0),
	SHELL_CMD_ARG(reset, NULL, 
		"Reset throughput statistics\n"
		"Usage: ble reset", 
		cmd_ble_reset, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(ble, &sub_ble, "BLE throughput test commands", NULL);

#endif /* CONFIG_BT_CUSTOM */