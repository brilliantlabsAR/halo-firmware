/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

/* This application demonstrates the communication and control of a device
 * allowing to remotely control an LED, and to transmit the state of a button.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "alif_ble.h"
#include "gapm.h"
#include "gap_le.h"
#include "gapc_le.h"
#include "gapc_sec.h"
#include "gapm_le.h"
#include "gapm_le_adv.h"
#include "gapm_le_scan.h"
#include "gapm_le_init.h"
#include "prf.h"
#include "gatt_db.h"
#include "gatt_srv.h"

#include "common.h"
#include "config.h"
#include "central.h"
#include "peripheral.h"
#include "gatt_client.h"

#define BT_CONN_STATE_CONNECTED    0x00
#define BT_CONN_STATE_DISCONNECTED 0x01

/* Load names from configuration file */
static const char device_name[] = CONFIG_BLE_TP_DEVICE_NAME;

#if CONFIG_BLE_BONDING
static struct app_con_bond_data {
	/* Bond data for specific address */
	gap_bdaddr_t addr;
	/* Corresponding keys */
	gapc_pairing_keys_t keys;
} app_con_bond_data[APP_CON_NB_MAX];
#endif

enum app_con_status_bits {
	APP_CON_STATUS_PAIRED = 1 << 0,
};

static struct app_con_info_ {
	/* Peer device address */
	gap_bdaddr_t addr;
	/* Connection index */
	uint8_t conidx;
	/* Status (see app_con_status enumeration) */
	uint8_t status_bf;
} app_con_info = {
	.conidx = GAP_INVALID_CONIDX,
};

/* Macros */
LOG_MODULE_REGISTER(main, LOG_LEVEL_ERR);
K_SEM_DEFINE(gapm_init_sem, 0, 1);

static enum app_state app_state;

enum gap_role tp_device_role;

static char *app_state_str[] = {
	"STANDBY",
	"INIT",
	"SCAN_START",
	"SCAN_ONGOING",
	"SCAN_READY",
	"PERIPHERAL_FOUND",
	"CONNECTING",
	"CONNECTED",
	"CONNECTED_PAIRED",
	"GET_FEATURES",
	"DISCOVER_SERVICES",
	"CENTRAL_READY",
	"DATA_TRANSMIT",
	"DATA_READ",
	"DATA_SEND_READY",
	"DATA_RECEIVE_READY",
	"STATS_RESET",
	"PERIPHERAL_START_ADVERTISING",
	"PERIPHERAL_RECEIVING",
	"PERIPHERAL_PREPARE_SENDING",
	"PERIPHERAL_SENDING",
	"PERIPHERAL_SEND_RESULTS",
	"DISCONNECT",
	"DISCONNECTED",
};

void app_transition_to(enum app_state const state)
{
	char const *p_from = app_state == APP_STATE_ERROR ? "ERROR" : app_state_str[app_state];
	char const *p_to = state == APP_STATE_ERROR ? "ERROR" : app_state_str[state];

	LOG_DBG("App transition to %s -> %s", p_from, p_to);
	app_state = state;
}

enum app_state get_app_state(void)
{
	return app_state;
}

uint8_t get_connection_index(void)
{
	return app_con_info.conidx;
}

uint16_t get_mtu_size(void)
{
	uint16_t mtu;

	alif_ble_mutex_lock(K_FOREVER);
	mtu = gatt_bearer_mtu_min_get(app_con_info.conidx);
	alif_ble_mutex_unlock();
	return mtu - GATT_NTF_HEADER_LEN;
}

/**
 * Bluetooth GAPM callbacks
 */
static void on_le_connection_req(uint8_t const conidx, uint32_t const metainfo,
				 uint8_t const actv_idx, uint8_t const role,
				 const gap_bdaddr_t *const p_peer_addr,
				 const gapc_le_con_param_t *const p_con_params,
				 uint8_t const clk_accuracy)
{
	LOG_INF("Connection request on index %u", conidx);

	LOG_DBG("Connection parameters: interval %u, latency %u, supervision timeout %u",
		p_con_params->interval, p_con_params->latency, p_con_params->sup_to);

	LOG_INF("Peer BD address %02X:%02X:%02X:%02X:%02X:%02X (conidx: %u)", p_peer_addr->addr[5],
		p_peer_addr->addr[4], p_peer_addr->addr[3], p_peer_addr->addr[2],
		p_peer_addr->addr[1], p_peer_addr->addr[0], conidx);

	gapc_bond_data_t bond_data;

	LOG_DBG("LE connection created: %s 0x%02X:%02X:%02X:%02X:%02X:%02X",
		(p_peer_addr->addr_type == 0) ? "Public" : "Private", p_peer_addr->addr[5],
		p_peer_addr->addr[4], p_peer_addr->addr[3], p_peer_addr->addr[2],
		p_peer_addr->addr[1], p_peer_addr->addr[0]);

	app_con_info.conidx = conidx;
	memcpy(&(app_con_info.addr.addr), p_peer_addr->addr, sizeof(p_peer_addr->addr));
	app_con_info.addr.addr_type = p_peer_addr->addr_type;

	if (app_con_info.status_bf & APP_CON_STATUS_PAIRED) {
		bond_data.enc_key_present = true;
		bond_data.pairing_lvl = GAP_PAIRING_BOND_PRESENT_BIT;
	}

	/* Automatically accept connection */
	gapc_le_connection_cfm(
		conidx, 0, (app_con_info.status_bf & APP_CON_STATUS_PAIRED) ? &bond_data : NULL);

	if (app_con_info.status_bf & APP_CON_STATUS_PAIRED) {
		app_transition_to(APP_STATE_CONNECTED_PAIRED);
	} else {
		app_transition_to(APP_STATE_CONNECTED);
	}
}

static void on_gapc_key_received(uint8_t const conidx, uint32_t const metainfo,
				 const gapc_pairing_keys_t *const p_keys)
{
	LOG_DBG("STORE KEY RECEIVED AS BOND DATA  %d", conidx);
#if CONFIG_BLE_BONDING
	if (conidx != app_con_info.conidx) {
		LOG_ERR("Invalid connection id!");
		assert(0);
	}

	app_con_bond_data.keys = *p_keys;
	app_con_bond_data.addr = app_con_info.addr;

	LOG_DBG("BOND Address : 0x%02X:%02X:%02X:%02X:%02X:%02X Valid_Key 0x%02X  Pairing_lvl = "
		"0x%02X",
		app_con_bond_data.addr.addr[5], app_con_bond_data.addr.addr[4],
		app_con_bond_data.addr.addr[3], app_con_bond_data.addr.addr[2],
		app_con_bond_data.addr.addr[1], app_con_bond_data.addr.addr[0],
		app_con_bond_data.keys.valid_key_bf, app_con_bond_data.keys.pairing_lvl);
#endif
}

static void on_gapc_le_encrypt_req(uint8_t const conidx, uint32_t const metainfo,
				   uint16_t const ediv, const gap_le_random_nb_t *const p_rand)
{
	LOG_DBG("ENCRYPT REQUEST %d", conidx);
#if CONFIG_BLE_BONDING
	if (conidx != app_con_info.conidx) {
		LOG_ERR("Invalid connection id!");
		assert(0);
	}
	gapc_le_encrypt_req_reply(conidx, true, &(app_con_bond_data.keys.ltk.key),
				  app_con_bond_data.keys.ltk.key_size);
#endif
}

/* Link authentication information */
static void on_gapc_sec_auth_info(uint8_t const conidx, uint32_t const metainfo,
				  uint8_t const sec_lvl, bool const encrypted,
				  uint8_t const key_size)
{
	LOG_DBG("AUTH INFO %d, %d-%s", conidx, sec_lvl, (encrypted ? "TRUE" : "FALSE"));
}

static void on_gapc_pairing_succeed(uint8_t const conidx, uint32_t const metainfo,
				    uint8_t const pairing_level, bool enc_key_present,
				    uint8_t const key_type)
{
	LOG_DBG("PAIRING SUCCEED %d", conidx);
	if (conidx != app_con_info.conidx) {
		LOG_ERR("Invalid connection id!");
		assert(0);
	}
	app_con_info.status_bf |= APP_CON_STATUS_PAIRED;

	app_transition_to(APP_STATE_CONNECTED_PAIRED);
}

/* Informed that pairing failed */
static void on_gapc_pairing_failed(uint8_t const conidx, uint32_t const metainfo,
				   uint16_t const reason)
{
	LOG_ERR("PAIRING FAILED %d, 0x%04X", conidx, reason);
}

static void on_gapc_info_req(uint8_t const conidx, uint32_t const metainfo, uint8_t const exp_info)
{
	switch (exp_info) {
	case GAPC_INFO_BT_PASSKEY: {
		LOG_DBG("PAIRING PASSKEY GET %d", conidx);
		/* Force 123456 PASS KEY */
#if CONFIG_BLE_BONDING
		gapc_pairing_provide_passkey(conidx, true, 123456);
#endif
		break;
	}
	default:
		break;
	}
}

static void on_gapc_pairing_req(uint8_t const conidx, uint32_t const metainfo,
				uint8_t const auth_level)
{
	LOG_DBG("PAIRING REQ %d", conidx);
#if CONFIG_BLE_BONDING
	uint16_t status;
	uint8_t sec_req_level = GAP_SEC1_NOAUTH_PAIR_ENC;
	gapc_pairing_t pairing_info = {
		.iocap = GAP_IO_CAP_NO_INPUT_NO_OUTPUT,
		.oob = GAP_OOB_AUTH_DATA_NOT_PRESENT,
		.auth = GAP_AUTH_BOND,
		.key_size = GAP_KEY_LEN,
		.ikey_dist = GAP_KDIST_ENCKEY | GAP_KDIST_IDKEY,
		.rkey_dist = GAP_KDIST_ENCKEY | GAP_KDIST_IDKEY,
	};

	gapm_le_configure_security_level(sec_req_level);
	status = gapc_le_pairing_accept(conidx, true, &pairing_info, 0);
	if (status != GAP_ERR_NO_ERROR) {
		ASSERT_INFO(0, status, 0);
	}
#endif
}

static void on_gapc_sec_numeric_compare_req(uint8_t const conidx, uint32_t const metainfo,
					    uint32_t const value)
{
	LOG_DBG("PAIRING USER VAL CFM %d %d", conidx, value);
	/* Automatically confirm */
#if CONFIG_BLE_BONDING
	gapc_pairing_numeric_compare_rsp(conidx, true);
#endif
}

static void on_gapc_sec_ltk_req(uint8_t const conidx, uint32_t const metainfo,
				uint8_t const key_size)
{
	LOG_DBG("LTK REQUEST %d", conidx);
#if CONFIG_BLE_BONDING
	if (conidx != app_con_info.conidx) {
		LOG_ERR("Invalid connection id!");
		assert(0);
	}

	if (!CHECKB(app_con_info.status_bf, APP_CON_STATUS_PAIRED)) {
		uint8_t cnt;

		app_con_bond_data.keys.ltk.key_size = GAP_KEY_LEN;
		app_con_bond_data.keys.ltk.ediv = (uint16_t)co_rand_word();

		for (cnt = 0; cnt < RAND_NB_LEN; cnt++) {
			app_con_bond_data.keys.ltk.key.key[cnt] = (uint8_t)co_rand_word();
			app_con_bond_data.keys.ltk.randnb.nb[cnt] = (uint8_t)co_rand_word();
		}

		for (cnt = RAND_NB_LEN; cnt < GAP_KEY_LEN; cnt++) {
			app_con_bond_data.keys.ltk.key.key[cnt] = (uint8_t)co_rand_word();
		}
	}

	gapc_le_pairing_provide_ltk(conidx, &(app_con_bond_data.keys.ltk));
#endif
}

static void on_disconnection(uint8_t const conidx, uint32_t const metainfo, uint16_t const reason)
{
	LOG_DBG("CONN disconnection idx=%d, meta=%u, reason=0x%04X", conidx, metainfo, reason);
	app_transition_to(APP_STATE_DISCONNECTED);
}

static void on_name_get(uint8_t const conidx, uint32_t const metainfo, uint16_t const token,
			uint16_t const offset, uint16_t const max_len)
{
	LOG_DBG("CONN name get idx=%u, meta=%u", conidx, metainfo);
	const size_t device_name_len = sizeof(device_name) - 1;
	const size_t short_len = (device_name_len > max_len ? max_len : device_name_len);
	const uint16_t status =
		((offset < device_name_len) ? GAP_ERR_NO_ERROR : ATT_ERR_INVALID_OFFSET);

	gapc_le_get_name_cfm(conidx, token, status, device_name_len, short_len,
			     (const uint8_t *)device_name);
}

static void on_appearance_get(uint8_t const conidx, uint32_t const metainfo, uint16_t const token)
{
	/* Send 'unknown' appearance */
	gapc_le_get_appearance_cfm(conidx, token, GAP_ERR_NO_ERROR, 0);
}

static void on_pref_param_get(uint8_t const conidx, uint32_t const metainfo, uint16_t const token)
{
	printk("%s\n", __func__);

	gapc_le_preferred_periph_param_t prefs = {
		.con_intv_min = 6,
		.con_intv_max = 200,
		.latency = 0,
		.conn_timeout = 1000,
	};

	gapc_le_get_preferred_periph_params_cfm(conidx, token, GAP_ERR_NO_ERROR, prefs);
}

static const gapc_connection_req_cb_t gapc_con_cbs = {
	.le_connection_req = on_le_connection_req,
};

static void on_param_update_req(uint8_t const conidx, uint32_t const metainfo,
				const gapc_le_con_param_nego_t *const p_param)
{
	LOG_DBG("%s:%d", __func__, conidx);
	gapc_le_update_params_cfm(conidx, true, 5, 10);
}

static void on_param_updated(uint8_t const conidx, uint32_t const metainfo,
			     const gapc_le_con_param_t *const p_param)
{
	LOG_DBG("%s: interval: %d, latency: %d, timeout: %d", __func__, p_param->interval,
		p_param->latency, p_param->sup_to);
}

static void on_packet_size_updated(uint8_t const conidx, uint32_t const metainfo,
				   uint16_t const max_tx_octets, uint16_t const max_tx_time,
				   uint16_t const max_rx_octets, uint16_t const max_rx_time)
{
	LOG_DBG("Packet size updated %u TX:%u max_tx_time:%d  max_rx_octets:%d "
		"max_rx_time:%d\n",
		conidx, max_tx_octets, max_tx_time, max_rx_octets, max_rx_time);
}

static void phy_updated(uint8_t const conidx, uint32_t const metainfo, uint8_t const tx_phy,
			uint8_t const rx_phy)
{
	LOG_DBG("PHY updated %u TX:%u RX:%u", conidx, tx_phy, rx_phy);
}

static const gapc_security_cb_t gapc_sec_cbs = {
	.le_encrypt_req = on_gapc_le_encrypt_req,
	.auth_info = on_gapc_sec_auth_info,
	.pairing_succeed = on_gapc_pairing_succeed,
	.pairing_failed = on_gapc_pairing_failed,
	.info_req = on_gapc_info_req,
	.pairing_req = on_gapc_pairing_req,
	.numeric_compare_req = on_gapc_sec_numeric_compare_req,
	.ltk_req = on_gapc_sec_ltk_req,
	.key_received = on_gapc_key_received,

	/* All other callbacks in this struct are optional */
};

static const gapc_connection_info_cb_t gapc_con_inf_cbs = {
	.disconnected = on_disconnection,
	.name_get = on_name_get,
	.appearance_get = on_appearance_get,
	/* Other callbacks in this struct are optional */
	.slave_pref_param_get = on_pref_param_get,
};

/* All callbacks in this struct are optional */
static const gapc_le_config_cb_t gapc_le_cfg_cbs = {
	.param_update_req = on_param_update_req,
	.param_updated = on_param_updated,
	.packet_size_updated = on_packet_size_updated,
	.phy_updated = phy_updated,
};

#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
static void on_gapm_err(uint32_t const metainfo, uint8_t const code)
{
	LOG_ERR("gapm error %d", code);
}

static const gapm_cb_t gapm_err_cbs = {
	.cb_hw_error = on_gapm_err,
};

#else /* ROM version 1.0 */
static void on_gapm_err(enum co_error const err)
{
	LOG_ERR("GAPM error %d", err);
}

static const gapm_err_info_config_cb_t gapm_err_cbs = {
	.ctrl_hw_error = on_gapm_err,
};

#endif

static const gapm_callbacks_t gapm_cbs = {
	.p_con_req_cbs = &gapc_con_cbs,
	.p_sec_cbs = &gapc_sec_cbs,
	.p_info_cbs = &gapc_con_inf_cbs,
	.p_le_config_cbs = &gapc_le_cfg_cbs,
	.p_bt_config_cbs = NULL,    /* BT classic so not required */
#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
	.p_gapm_cbs = &gapm_err_cbs,
#else
	.p_err_info_config_cbs = &gapm_err_cbs,
#endif
};

/* ---------------------------------------------------------------------------------------- */
/* BLE config (GAPM) */

void on_gapm_process_complete(uint32_t const metainfo, uint16_t const status)
{
	if (status) {
		LOG_ERR("gapm process completed with error %u", status);
		app_transition_to(APP_STATE_ERROR);
		return;
	}

	LOG_DBG("gapm process completed successfully");

	k_sem_give(&gapm_init_sem);
}

static int ble_configure(uint8_t const role)
{
	LOG_DBG("Configuring BLE to role %s", role == GAP_ROLE_LE_CENTRAL ? "CENTRAL" : "PERIPH");

	gap_addr_t private_address = {
		.addr = {0xCF, 0xFE, 0xFB, 0xDE, 0x11, 0x0},
	};

	/* Set Random Static Address */
	if (role == GAP_ROLE_LE_ALL) {
		private_address.addr[5] = 0x06;
	} else if (role == GAP_ROLE_LE_CENTRAL) {
		private_address.addr[5] = 0x07;
	} else {
		private_address.addr[5] = 0x08;
	}

	/* Bluetooth stack configuration*/
	gapm_config_t gapm_cfg = {
		.role = role,
		/* -------------- Security Config ------------------------------------ */
		.pairing_mode = GAPM_PAIRING_DISABLE,

		/* -------------- Privacy Config ------------------------------------- */
		.privacy_cfg = GAPM_PRIV_CFG_PRIV_ADDR_BIT,
		.renew_dur = 1500,
		.private_identity = private_address,

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

	int err = gapm_configure(0, &gapm_cfg, &gapm_cbs, on_gapm_process_complete);

	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("gapm_configure error %u", err);
		return -1;
	}

	if (k_sem_take(&gapm_init_sem, K_MSEC(200) /*K_FOREVER*/) != 0) {
		LOG_ERR("gapm_configure not ready in 200ms");
		return -1;
	}

	err = gapm_set_name(0, strlen(device_name), (const uint8_t *)device_name, NULL);
	if (err != GAP_ERR_NO_ERROR) {
		gap_bdaddr_t identity;

		gapm_get_identity(&identity);

		LOG_DBG("Device address: %02X:%02X:%02X:%02X:%02X:%02X", identity.addr[5],
			identity.addr[4], identity.addr[3], identity.addr[2], identity.addr[1],
			identity.addr[0]);
	}

	LOG_DBG("GAPM init complete!");

	return 0;
}

int get_private_address(char *p_str, size_t const len)
{
	if (!p_str || len < 18) {
		return -EINVAL;
	}
	gap_bdaddr_t identity;

	gapm_get_identity(&identity);
	snprintk(p_str, len, "%02X:%02X:%02X:%02X:%02X:%02X", identity.addr[5], identity.addr[4],
		 identity.addr[3], identity.addr[2], identity.addr[1], identity.addr[0]);
	return 0;
}

int convert_uuid_to_string(char *const p_str, size_t const max_len, uint8_t const *const p_uuid,
			   uint8_t const uuid_type)
{
	if (!p_uuid || !max_len) {
		return -EINVAL;
	}
	switch (uuid_type) {
	case GATT_UUID_16:
		snprintk(p_str, max_len, "%02x%02x", p_uuid[0], p_uuid[1]);
		break;
	case GATT_UUID_32:
		snprintk(p_str, max_len, "%02x%02x%02x%02x", p_uuid[0], p_uuid[1], p_uuid[2],
			 p_uuid[3]);
		break;
	case GATT_UUID_128:
		snprintk(p_str, max_len,
			 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x"
			 "%02x%02x%02x",
			 p_uuid[0], p_uuid[1], p_uuid[2], p_uuid[3], p_uuid[4], p_uuid[5],
			 p_uuid[6], p_uuid[7], p_uuid[8], p_uuid[9], p_uuid[10], p_uuid[11],
			 p_uuid[12], p_uuid[13], p_uuid[14], p_uuid[15]);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

int convert_uuid_with_len_to_string(char *const p_str, size_t const max_len,
				    uint8_t const *const p_uuid, size_t const uuid_len)
{
	uint8_t uuid_type = 0;

	if (GATT_UUID_128_LEN == uuid_len) {
		uuid_type = GATT_UUID_128;
	} else if (GATT_UUID_32_LEN == uuid_len) {
		uuid_type = GATT_UUID_32;
	} else if (GATT_UUID_16_LEN == uuid_len) {
		uuid_type = GATT_UUID_16;
	} else {
		return -EINVAL;
	}
	return convert_uuid_to_string(p_str, max_len, p_uuid, uuid_type);
}

void tp_worker(void *p1, void *p2, void *p3)
{
	LOG_DBG("Starting throughput app...");

	/* Start up bluetooth host stack */
	alif_ble_enable(NULL);

	app_state = APP_STATE_INIT;

	while (1) {
		switch (app_state) {
		case APP_STATE_INIT: {
			if (ble_configure(tp_device_role)) {
				LOG_ERR("ble_configure failed");
				break;
			}

			if (tp_device_role == GAP_ROLE_LE_CENTRAL) {
				central_app_init();
				app_transition_to(APP_STATE_SCAN_START);
			} else if (tp_device_role == GAP_ROLE_LE_PERIPHERAL) {
				peripheral_app_init();
				app_transition_to(APP_STATE_PERIPHERAL_START_ADVERTISING);
			} else {
				LOG_ERR("Unsupported device role");
			}
			break;
		}
		case APP_STATE_ERROR: {
			LOG_ERR("Error, set state to DISCONNECTED");

			if ((tp_device_role == GAP_ROLE_LE_CENTRAL) ||
			    (tp_device_role == GAP_ROLE_LE_PERIPHERAL)) {
				app_transition_to(APP_STATE_DISCONNECTED);
			} else {
				LOG_ERR("Unsupported device role");
			}
		}
		case APP_STATE_STANDBY: {
			k_sleep(K_MSEC(100));
			break;
		}
		default: {
			if (tp_device_role == GAP_ROLE_LE_CENTRAL) {
				if (0 == central_app_exec(app_state)) {
					break;
				}
			}
			if (tp_device_role == GAP_ROLE_LE_PERIPHERAL) {
				if (0 == peripheral_app_exec(app_state)) {
					break;
				}
			}
			LOG_ERR("Invalid state = %d", app_state);
			break;
		}
		};
	}
}

void set_device_role(enum gap_role role)
{
	tp_device_role = role;
}

enum gap_role get_device_role(void)
{
	return tp_device_role;
}