/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "alif_ble.h"
#include "gap_le.h"
#include "gapc_le.h"
#include "gapc_sec.h"
#include "gapm.h"
#include "gapm_le.h"
#include "gapm_le_adv.h"
#include "gapm_le_per_sync.h"
#include "address_verification.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#define DEVICE_NAME CONFIG_BLE_DEVICE_NAME

/* Define advertising address type */
#define SAMPLE_ADDR_TYPE	ALIF_STATIC_RAND_ADDR

/* Store and share advertising address type */
static uint8_t adv_type;

static uint8_t adv_actv_idx;
static uint8_t sync_actv_idx;

static uint16_t utils_start_per_adv_sync(uint8_t conidx)
{
	const gapm_le_per_sync_param_t sync_params = {
		.skip = 0,
		.sync_to = 1000,
		.type = GAPM_PER_SYNC_TYPE_PAST,
		.conidx = conidx,
		.adv_addr = {},
		.report_en_bf = GAPM_REPORT_ADV_EN_BIT,
		.cte_type = 0,
	};

	return gapm_le_start_per_sync(sync_actv_idx, &sync_params);
}

static uint16_t utils_add_ltv_field(uint8_t *p_buf, uint16_t *p_len, uint8_t type,
				    const void *p_val, uint8_t val_len)
{
	if ((*p_len + GAP_AD_HEADER_SIZE + val_len) > GAP_ADV_DATA_LEN) {
		return GAP_ERR_INSUFF_RESOURCES;
	}

	p_buf[(*p_len)++] = val_len + GAP_AD_TYPE_SIZE;
	p_buf[(*p_len)++] = type;
	memcpy(&(p_buf[*p_len]), p_val, val_len);
	*p_len += val_len;

	return GAP_ERR_NO_ERROR;
}

static uint16_t utils_set_adv_data(const uint8_t *p_buf, uint16_t len)
{
	co_buf_t *p_co_buf;
	uint16_t rc;

	if (co_buf_alloc(&p_co_buf, 0, len, 0) != CO_BUF_ERR_NO_ERROR) {
		return GAP_ERR_INSUFF_RESOURCES;
	}

	co_buf_copy_data_from_mem(p_co_buf, p_buf, len);
	rc = gapm_le_set_adv_data(adv_actv_idx, p_co_buf);
	co_buf_release(p_co_buf);

	return rc;
}

static uint16_t utils_set_scan_resp_data(const uint8_t *p_buf, uint16_t len)
{
	co_buf_t *p_co_buf;
	uint16_t rc;

	if (co_buf_alloc(&p_co_buf, 0, len, 0) != CO_BUF_ERR_NO_ERROR) {
		return GAP_ERR_INSUFF_RESOURCES;
	}

	co_buf_copy_data_from_mem(p_co_buf, p_buf, len);
	rc = gapm_le_set_scan_response_data(adv_actv_idx, p_co_buf);
	co_buf_release(p_co_buf);

	return rc;
}

static uint16_t utils_create_adv_data(void)
{
	uint16_t adv_len = 0;
	uint8_t adv_buf[GAP_ADV_DATA_LEN] = {0};
	uint16_t rc;

	rc = utils_add_ltv_field(adv_buf, &adv_len, GAP_AD_TYPE_COMPLETE_NAME, DEVICE_NAME,
				 strlen(DEVICE_NAME));
	if (rc != GAP_ERR_NO_ERROR) {
		return rc;
	}

	return utils_set_adv_data(adv_buf, adv_len);
}

static uint16_t utils_start_adv(void)
{
	static const gapm_le_adv_param_t params = {
		.duration = 0,
	};

	return gapm_le_start_adv(adv_actv_idx, &params);
}

static void on_adv_actv_proc_cmp(uint32_t metainfo, uint8_t proc_id, uint8_t actv_idx,
				 uint16_t status)
{
	uint16_t rc;

	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Advertising completion callback failed, error: %u", status);
		return;
	}

	switch (proc_id) {
	case GAPM_ACTV_CREATE_LE_ADV: {
		rc = utils_create_adv_data();
		if (rc != GAP_ERR_NO_ERROR) {
			LOG_ERR("Failed to create advertisement data, error: %u", rc);
			return;
		}
	} break;

	case GAPM_ACTV_SET_ADV_DATA: {
		rc = utils_set_scan_resp_data(NULL, 0);
		if (rc != GAP_ERR_NO_ERROR) {
			LOG_ERR("Failed to set scan data, error: %u", rc);
			return;
		}
	} break;

	case GAPM_ACTV_SET_SCAN_RSP_DATA: {
		rc = utils_start_adv();
		if (rc != GAP_ERR_NO_ERROR) {
			LOG_ERR("Failed to start advertising, error: %u", rc);
			return;
		}
	} break;

	case GAPM_ACTV_START: {
		print_device_identity();
		address_verification_log_advertising_address(actv_idx);
	} break;

	default: {
		LOG_WRN("Unhandled advertising state: %u", proc_id);
	} break;
	}
}

static void on_adv_actv_stopped(uint32_t metainfo, uint8_t actv_idx, uint16_t reason)
{
	LOG_INF("Advertising has been stopped");
}

static void on_adv_created(uint32_t metainfo, uint8_t actv_idx, int8_t tx_pwr)
{
	adv_actv_idx = actv_idx;
}

static void on_le_connection_req(uint8_t conidx, uint32_t metainfo, uint8_t actv_idx, uint8_t role,
				 const gap_bdaddr_t *p_peer_addr,
				 const gapc_le_con_param_t *p_con_params, uint8_t clk_accuracy)
{
	uint16_t rc;

	rc = gapc_le_connection_cfm(conidx, 0, NULL);
	if (rc != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to accept incoming connection, error: %u", rc);
		return;
	}

	LOG_INF("New client connection from %02X:%02X:%02X:%02X:%02X:%02X (conidx: %u)",
		p_peer_addr->addr[5], p_peer_addr->addr[4], p_peer_addr->addr[3],
		p_peer_addr->addr[2], p_peer_addr->addr[1], p_peer_addr->addr[0], conidx);

	rc = utils_start_per_adv_sync(conidx);
	if (rc != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to start periodic advertising sync (conidx: %u), error: %u", conidx,
			rc);
		return;
	}

	LOG_INF("Started periodic advertising sync (conidx: %u)", conidx);
}

static void on_key_received(uint8_t conidx, uint32_t metainfo, const gapc_pairing_keys_t *p_keys)
{
	LOG_WRN("Received unexpected pairing key from conidx: %u", conidx);
}

static void on_disconnection(uint8_t conidx, uint32_t metainfo, uint16_t reason)
{
	uint16_t rc;

	LOG_INF("Client disconnected (conidx: %u), restarting advertising", conidx);

	rc = utils_start_adv();
	if (rc != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to restart advertising, error: %u", rc);
		return;
	}
}

static void on_name_get(uint8_t conidx, uint32_t metainfo, uint16_t token, uint16_t offset,
			uint16_t max_len)
{
	LOG_WRN("Received unexpected name get from conidx: %u", conidx);
}

static void on_appearance_get(uint8_t conidx, uint32_t metainfo, uint16_t token)
{
	uint16_t rc;

	/* User must implement .appearance_get callback if appearance is not set using
	 * gapm_le_set_appearance or if appearance set is unknown
	 */
	rc = gapc_le_get_appearance_cfm(conidx, token, GAP_ERR_NO_ERROR, 0);
	if (rc != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to send appearance error: %u", rc);
		return;
	}
}

#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
static void on_gapm_err(uint32_t metainfo, uint8_t code)
{
	LOG_ERR("gapm error %d", code);
}
#else
static void on_gapm_err(enum co_error err)
{
	LOG_ERR("gapm error %d", err);
}
#endif /* !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 */

static uint16_t utils_create_adv(void)
{
	static const gapm_le_adv_create_param_t adv_create_params = {
		.prop = GAPM_ADV_PROP_UNDIR_CONN_MASK,
		.disc_mode = GAPM_ADV_MODE_GEN_DISC,
#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
		.tx_pwr = 0,
#else
		.max_tx_pwr = 0,
#endif /* !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 */
		.filter_pol = GAPM_ADV_ALLOW_SCAN_ANY_CON_ANY,
		.prim_cfg = {
				.adv_intv_min = 160,
				.adv_intv_max = 800,
				.ch_map = ADV_ALL_CHNLS_EN,
				.phy = GAPM_PHY_TYPE_LE_1M,
			},
	};

	static const gapm_le_adv_cb_actv_t le_adv_cbs = {
		.hdr.actv.proc_cmp = on_adv_actv_proc_cmp,
		.hdr.actv.stopped = on_adv_actv_stopped,
		.created = on_adv_created,
	};

	return gapm_le_create_adv_legacy(0, adv_type, &adv_create_params, &le_adv_cbs);
}

static void on_per_adv_proc_cmp(uint32_t metainfo, uint8_t proc_id, uint8_t actv_idx,
				uint16_t status)
{
	switch (proc_id) {
	case GAPM_ACTV_START:
		LOG_INF("Periodic advertising sync activity has been started");
		break;

	case GAPM_ACTV_STOP:
		LOG_INF("Periodic advertising sync activity has been stopped");
		break;
	}
}

static void on_per_adv_stopped(uint32_t metainfo, uint8_t actv_idx, uint16_t reason)
{
	if (reason == GAP_ERR_DISCONNECTED) {
		LOG_ERR("Periodic advertising sync lost");
		return;
	}

	LOG_INF("Periodic advertising sync stopped");
}

static void on_report_received(uint32_t metainfo, uint8_t actv_idx,
			       const gapm_le_adv_report_info_t *p_info, co_buf_t *p_report)
{
	LOG_INF("Periodic advertising report received");

	LOG_INF("trans_addr: %02X:%02X:%02X:%02X:%02X:%02X addr_type: %u",
		p_info->trans_addr.addr[5], p_info->trans_addr.addr[4], p_info->trans_addr.addr[3],
		p_info->trans_addr.addr[2], p_info->trans_addr.addr[1], p_info->trans_addr.addr[0],
		p_info->trans_addr.addr_type);

	LOG_INF("target_addr: %02X:%02X:%02X:%02X:%02X:%02X addr_type: %u",
		p_info->target_addr.addr[5], p_info->target_addr.addr[4],
		p_info->target_addr.addr[3], p_info->target_addr.addr[2],
		p_info->target_addr.addr[1], p_info->target_addr.addr[0],
		p_info->target_addr.addr_type);

	LOG_INF("info: %u, tx_pwr: %i rssi: %i, phy_prim: %u, phy_second: %u adv_sid: %u, "
		"period_adv_intv: %u",
		p_info->info, p_info->tx_pwr, p_info->rssi, p_info->phy_prim, p_info->phy_second,
		p_info->adv_sid, p_info->period_adv_intv);

	LOG_HEXDUMP_INF(co_buf_data(p_report), co_buf_data_len(p_report), "p_report:");
}

static void on_established(uint32_t metainfo, uint8_t actv_idx,
			   const gapm_le_per_sync_info_t *p_info)
{
	LOG_INF("Periodic advertising sync established");

	LOG_INF("addr: %02X:%02X:%02X:%02X:%02X:%02X addr_type: %u", p_info->addr.addr[5],
		p_info->addr.addr[4], p_info->addr.addr[3], p_info->addr.addr[2],
		p_info->addr.addr[1], p_info->addr.addr[0], p_info->addr.addr_type);

	LOG_INF("phy: %u, interval: %u, adv_sid: %u, clk_acc: %u, serv_data: %u", p_info->phy,
		p_info->interval, p_info->adv_sid, p_info->clk_acc, p_info->serv_data);
}

static uint16_t utils_create_per_sync(void)
{
	static const gapm_le_per_sync_cb_actv_t sync_cbs = {
		.actv.proc_cmp = on_per_adv_proc_cmp,
		.actv.stopped = on_per_adv_stopped,
		.report_received = on_report_received,
		.established = on_established,
	};

	return gapm_le_create_per_sync(0, &sync_cbs, &sync_actv_idx);
}

static void on_gapm_name_proc_cmp_cb(uint32_t metainfo, uint16_t status)
{
	uint16_t rc;

	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("GAPM name set callback failed, error: %u", status);
		return;
	}

	LOG_INF("Creating periodic sync");
	rc = utils_create_per_sync();
	if (rc != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to create periodic sync, error: %u", rc);
		return;
	}

	LOG_INF("Creating advertisement");
	rc = utils_create_adv();
	if (rc != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to create advertising activity, error: %u", rc);
		return;
	}
}

static void on_gapm_process_complete(uint32_t metainfo, uint16_t status)
{
	uint16_t rc;

	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("GAPM completion callback failed, error: %u", status);
		return;
	}

	LOG_INF("Setting device name: %s", DEVICE_NAME);
	rc = gapm_set_name(0, strlen(DEVICE_NAME), DEVICE_NAME, on_gapm_name_proc_cmp_cb);
	if (rc != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to set device name, error: %u", rc);
		return;
	}
}

static uint16_t utils_config_gapm(void)
{
	static gapm_config_t gapm_cfg = {
		/* Observer role is needed for periodic sync */
		.role = GAP_ROLE_LE_PERIPHERAL | GAP_ROLE_LE_OBSERVER,
		.pairing_mode = GAPM_PAIRING_DISABLE,
		.pairing_min_req_key_size = 0,
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
		.class_of_device = 0,
		.dflt_link_policy = 0,
	};

	if (address_verification(SAMPLE_ADDR_TYPE, &adv_type, &gapm_cfg)) {
		LOG_ERR("Address verification failed");
		return -EADV;
	}

	static const gapc_connection_req_cb_t gapc_con_cbs = {
		.le_connection_req = on_le_connection_req,
	};

	static const gapc_security_cb_t gapc_sec_cbs = {
		.key_received = on_key_received,
	};

	static const gapc_connection_info_cb_t gapc_con_inf_cbs = {
		.disconnected = on_disconnection,
		.name_get = on_name_get,
		.appearance_get = on_appearance_get,
	};

	static const gapc_le_config_cb_t gapc_le_cfg_cbs = {};

#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
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
#else
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
#endif /* !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 */

	return gapm_configure(0, &gapm_cfg, &gapm_cbs, on_gapm_process_complete);
}

static void on_ble_enabled(void)
{
	uint16_t rc;

	LOG_INF("Configuring GAP manager");
	rc = utils_config_gapm();
	if (rc != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to configure GAP, error: %u", rc);
		return;
	}
}

int main(void)
{
	int rc;

	LOG_INF("Enabling Alif BLE stack");
	rc = alif_ble_enable(on_ble_enabled);
	if (rc) {
		LOG_ERR("Failed to enable Alif BLE stack, error: %i", rc);
		return -1;
	}

	while (1) {
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
