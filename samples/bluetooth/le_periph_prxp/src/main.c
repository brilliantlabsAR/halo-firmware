/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

/*
 * This example will start an instance of a peripheral Proximity Profile (PRXP)
 * it will notify when the link has been lost with the alert level set from the
 * central device. Includes Battery Service support.
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
#include "co_buf.h"
#include "address_verification.h"

/*  Profiles definitions */
#include "batt_svc.h"
#include "shared_control.h"
#include "prf.h"
#include "bass.h"

#include "prxp_app.h"

static uint16_t start_le_adv(uint8_t actv_idx);

K_SEM_DEFINE(init_sem, 0, 1);

/* Define advertising address type */
#define SAMPLE_ADDR_TYPE	ALIF_STATIC_RAND_ADDR

/* Store and share advertising address type */
static uint8_t adv_type;

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

/* Bluetooth stack configuration */
static gapm_config_t gapm_cfg = {
	.role = GAP_ROLE_LE_PERIPHERAL,
	.pairing_mode = GAPM_PAIRING_DISABLE,
	.privacy_cfg = 0,
	.renew_dur = 1500,
	.private_identity.addr = {0, 0, 0, 0, 0, 0},
	.irk.key = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
	.gap_start_hdl = 0,
	.gatt_start_hdl = 0,
	.att_cfg = 0,
	.sugg_max_tx_octets = GAP_LE_MIN_OCTETS,
	.sugg_max_tx_time = GAP_LE_MIN_TIME,
	.tx_pref_phy = GAP_PHY_ANY,
	.rx_pref_phy = GAP_PHY_ANY,
	.tx_path_comp = 0,
	.rx_path_comp = 0,
};

static struct shared_control ctrl = {false, 0, 0};

/* Load name from configuration file */
#define DEVICE_NAME CONFIG_BLE_DEVICE_NAME
static const char device_name[] = DEVICE_NAME;

static uint8_t adv_actv_idx;

/**
 * Bluetooth GAPM callbacks
 */
static void on_le_connection_req(uint8_t conidx, uint32_t metainfo, uint8_t actv_idx, uint8_t role,
				 const gap_bdaddr_t *p_peer_addr,
				 const gapc_le_con_param_t *p_con_params, uint8_t clk_accuracy)
{
	LOG_INF("Connection request on index %u", conidx);
	gapc_le_connection_cfm(conidx, 0, NULL);

	LOG_DBG("Connection parameters: interval %u, latency %u, supervision timeout %u",
		p_con_params->interval, p_con_params->latency, p_con_params->sup_to);

	LOG_HEXDUMP_DBG(p_peer_addr->addr, GAP_BD_ADDR_LEN, "Peer BD address");

	LOG_DBG("Please enable notifications on peer device..");

	ctrl.connected = true;
}

static void on_key_received(uint8_t conidx, uint32_t metainfo, const gapc_pairing_keys_t *p_keys)
{
	LOG_WRN("Unexpected key received key on conidx %u", conidx);
}

static void on_disconnection(uint8_t conidx, uint32_t metainfo, uint16_t reason)
{
	uint16_t err;

	LOG_INF("Connection index %u disconnected for reason %u", conidx, reason);
	err = start_le_adv(adv_actv_idx);
	if (err) {
		LOG_ERR("Error restarting advertising: %u", err);
	} else {
		LOG_DBG("Restarting advertising");
	}

	disc_notify(reason);
	/* Update shared control */
	ctrl.connected = false;
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
	/* Send unknown appearance */
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

static const gapm_callbacks_t get_cbs(void)
{
	gapm_callbacks_t ret = {
		.p_con_req_cbs = &gapc_con_cbs,
		.p_sec_cbs = &gapc_sec_cbs,
		.p_info_cbs = &gapc_con_inf_cbs,
		.p_le_config_cbs = &gapc_le_cfg_cbs,
		.p_bt_config_cbs = NULL /* BT classic so not required */
	};
	ret = append_cbs(&ret);

	return ret;
}

/*
 *	Advertising functions
 */

static uint8_t adv_actv_idx;

static uint16_t start_le_adv(uint8_t actv_idx)
{
	uint16_t err;
	gapm_le_adv_param_t adv_params = {
		.duration = 0, /* Advertise indefinitely */
	};

	err = gapm_le_start_adv(actv_idx, &adv_params);
	if (err) {
		LOG_ERR("Failed to start LE advertising with error %u", err);
	}
	return err;
}

static uint16_t set_advertising_data(uint8_t actv_idx)
{
	uint16_t err;
	uint16_t svc;

	/* gatt service identifier */
	svc = GATT_SVC_LINK_LOSS;
	uint16_t svc2 = get_batt_id();

	uint8_t num_svc = 2;
	const size_t device_name_len = sizeof(device_name) - 1;
	const uint16_t adv_device_name = GATT_HANDLE_LEN + device_name_len;
	const uint16_t adv_uuid_svc = GATT_HANDLE_LEN + (GATT_UUID_16_LEN * num_svc);

	/* Create advertising data with necessary services */
	const uint16_t adv_len = adv_device_name + adv_uuid_svc;

	co_buf_t *p_buf;

	err = co_buf_alloc(&p_buf, 0, adv_len, 0);
	__ASSERT(err == 0, "Buffer allocation failed");

	uint8_t *p_data = co_buf_data(p_buf);

	p_data[0] = device_name_len + 1;
	p_data[1] = GAP_AD_TYPE_COMPLETE_NAME;
	memcpy(p_data + 2, device_name, device_name_len);

	/* Update data pointer */
	p_data = p_data + adv_device_name;
	p_data[0] = (GATT_UUID_16_LEN * num_svc) + 1;
	p_data[1] = GAP_AD_TYPE_COMPLETE_LIST_16_BIT_UUID;

	/* Copy identifier */
	memcpy(p_data + 2, (void *)&svc, sizeof(svc));
	memcpy(p_data + 4, (void *)&svc2, sizeof(svc2));

	err = gapm_le_set_adv_data(actv_idx, p_buf);
	co_buf_release(p_buf); /* Release ownership of buffer so stack can free it when done */
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
	co_buf_release(p_buf); /* Release ownership of buffer so stack can free it when done */
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
		print_device_identity();
		address_verification_log_advertising_address(actv_idx);
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

uint16_t create_advertising(void)
{
	uint16_t err;
	gapm_le_adv_create_param_t adv_params;

	gapm_le_adv_create_param_t adv_create_params = {
		.prop = GAPM_ADV_PROP_UNDIR_CONN_MASK,
		.disc_mode = GAPM_ADV_MODE_GEN_DISC,
		.filter_pol = GAPM_ADV_ALLOW_SCAN_ANY_CON_ANY,
		.prim_cfg = {
				.adv_intv_min = 160, /* 100 ms */
				.adv_intv_max = 800, /* 500 ms */
				.ch_map = ADV_ALL_CHNLS_EN,
				.phy = GAPM_PHY_TYPE_LE_1M,
			},
	};

	adv_params = append_adv_param(&adv_create_params);

	err = gapm_le_create_adv_legacy(0, adv_type, &adv_params, &le_adv_cbs);
	if (err) {
		LOG_ERR("Error %u creating advertising activity", err);
	}

	return err;
}

static void on_gapm_process_complete(uint32_t metainfo, uint16_t status)
{
	if (status) {
		LOG_ERR("gapm process completed with error %u", status);
		return;
	}

	LOG_DBG("gapm process completed successfully");
	k_sem_give(&init_sem);
}

int main(void)
{
	uint16_t err;
	gapm_callbacks_t gapm_cbs;

	/* Start up bluetooth host stack */
	alif_ble_enable(NULL);

	gapm_cbs = get_cbs();

	if (address_verification(SAMPLE_ADDR_TYPE, &adv_type, &gapm_cfg)) {
		LOG_ERR("Address verification failed");
		return -EADV;
	}

	err = gapm_configure(0, &gapm_cfg, &gapm_cbs, on_gapm_process_complete);
	if (err) {
		LOG_ERR("gapm_configure error %u", err);
		return err;
	}

	LOG_DBG("Waiting for init...\n");

	k_sem_take(&init_sem, K_FOREVER);

	LOG_DBG("Init complete!\n");

	/* Share control structure */
	service_conn(&ctrl);

	/* Adding battery service */
	config_battery_service();

	create_advertising();

	server_configure();

	while (1) {
		k_sleep(K_SECONDS(2));
		battery_process();
		ias_process();
	}
	return -EINVAL;
}
