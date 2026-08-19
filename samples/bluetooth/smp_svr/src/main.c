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
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <mgmt/mcumgr/transport/smp_internal.h>

#include "alif_ble.h"
#include "gap_le.h"
#include "gapc_le.h"
#include "gapc_sec.h"
#include "gapm.h"
#include "gapm_le.h"
#include "gapm_le_adv.h"
#include "co_endian.h"
#include "gatt_db.h"
#include "batt_svc.h"
#include "shared_control.h"
#include "address_verification.h"

extern void service_conn(struct shared_control *ctrl);
struct shared_control ctrl = { false, 0, 0 };

/* Define advertising address type */
#define SAMPLE_ADDR_TYPE	ALIF_PUBLIC_ADDR

/* Store and share advertising address type */
static uint8_t adv_type;

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#define DEVICE_NAME CONFIG_BLE_DEVICE_NAME

/* Standard GATT 16 bit UUIDs must be extended to 128 bits when using gatt_att_desc_t */
#define GATT_DECL_PRIMARY_SERVICE_UUID128                                                          \
	{GATT_DECL_PRIMARY_SERVICE & 0xFF,                                                         \
	 (GATT_DECL_PRIMARY_SERVICE >> 8) & 0xFF,                                                  \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0}

#define GATT_DECL_CHARACTERISTIC_UUID128                                                           \
	{GATT_DECL_CHARACTERISTIC & 0xFF,                                                          \
	 (GATT_DECL_CHARACTERISTIC >> 8) & 0xFF,                                                   \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0}

#define GATT_DESC_CLIENT_CHAR_CFG_UUID128                                                          \
	{GATT_DESC_CLIENT_CHAR_CFG & 0xFF,                                                         \
	 (GATT_DESC_CLIENT_CHAR_CFG >> 8) & 0xFF,                                                  \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0,                                                                                        \
	 0}

/* SMP service 8D53DC1D-1DB7-4CD3-868B-8A527460AA84 */
#define SMP_SERVICE_UUID128                                                                        \
	{0x84, 0xAA, 0x60, 0x74, 0x52, 0x8A, 0x8B, 0x86,                                           \
	 0xD3, 0x4C, 0xB7, 0x1D, 0x1D, 0xDC, 0x53, 0x8D}

/* SMP characteristic DA2E7828-FBCE-4E01-AE9E-261174997C48 */
#define SMP_CHARACTERISTIC_UUID128                                                                 \
	{0x48, 0x7C, 0x99, 0x74, 0x11, 0x26, 0x9E, 0xAE,                                           \
	 0x01, 0x4E, 0xCE, 0xFB, 0x28, 0x78, 0x2E, 0xDA}

enum smp_gatt_id {
	SMP_GATT_ID_SERVICE = 0,
	SMP_GATT_ID_CHAR,
	SMP_GATT_ID_VAL,
	SMP_GATT_ID_NTF_CFG,
	SMP_GATT_ID_END,
};

struct smp_environment {
	uint8_t conidx;
	uint8_t adv_actv_idx;
	uint16_t ntf_cfg;
	uint16_t start_hdl;
	uint8_t user_lid;
	struct k_sem ntf_sem;
	struct smp_transport transport;
};

static struct smp_environment env;

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
	rc = gapm_le_set_adv_data(env.adv_actv_idx, p_co_buf);
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
	rc = gapm_le_set_scan_response_data(env.adv_actv_idx, p_co_buf);
	co_buf_release(p_co_buf);

	return rc;
}

static uint16_t utils_create_adv_data(void)
{
	const uint8_t svc_uuid[GATT_UUID_128_LEN] = SMP_SERVICE_UUID128;
	uint16_t adv_len = 0;
	uint8_t adv_buf[GAP_ADV_DATA_LEN] = {0};
	uint16_t rc;

	rc = utils_add_ltv_field(adv_buf, &adv_len, GAP_AD_TYPE_COMPLETE_NAME, DEVICE_NAME,
				 strlen(DEVICE_NAME));
	if (rc != GAP_ERR_NO_ERROR) {
		return rc;
	}

	rc = utils_add_ltv_field(adv_buf, &adv_len, GAP_AD_TYPE_COMPLETE_LIST_128_BIT_UUID,
				 &svc_uuid, GATT_UUID_128_LEN);
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

	return gapm_le_start_adv(env.adv_actv_idx, &params);
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
	env.adv_actv_idx = actv_idx;
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

	env.conidx = conidx;
	ctrl.connected = true;
}

static void on_key_received(uint8_t conidx, uint32_t metainfo, const gapc_pairing_keys_t *p_keys)
{
	LOG_WRN("Received unexpected pairing key from conidx: %u", conidx);
}

static void on_disconnection(uint8_t conidx, uint32_t metainfo, uint16_t reason)
{
	uint16_t rc;

	LOG_INF("Client disconnected (conidx: %u), restating advertising", conidx);

	smp_rx_remove_invalid(&env.transport, NULL);

	env.conidx = GAP_INVALID_CONIDX;
	env.ntf_cfg = PRF_CLI_STOP_NTFIND;
	k_sem_give(&env.ntf_sem);

	ctrl.connected = false;

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

static void on_cb_event_sent(uint8_t conidx, uint8_t user_lid, uint16_t metainfo, uint16_t status)
{
	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Notification send callback failed, status: %u", status);
	}

	k_sem_give(&env.ntf_sem);
}

static void on_cb_att_read_get(uint8_t conidx, uint8_t user_lid, uint16_t token, uint16_t hdl,
			       uint16_t offset, uint16_t max_length)
{
	uint16_t rc;
	co_buf_t *p_buf = NULL;
	uint16_t idx = hdl - env.start_hdl;

	switch (idx) {
	case SMP_GATT_ID_NTF_CFG:
		rc = co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, PRF_CCC_DESC_LEN,
				  GATT_BUFFER_TAIL_LEN);
		if (rc != CO_BUF_ERR_NO_ERROR) {
			rc = GAP_ERR_INSUFF_RESOURCES;
			break;
		}

		co_write16(co_buf_data(p_buf), co_htole16(env.ntf_cfg));

		LOG_INF("Value read notification configuration (conidx: %u), config: %u", conidx,
			env.ntf_cfg);
		rc = GAP_ERR_NO_ERROR;
		break;

	default:
		LOG_ERR("Value read to unknown characteristic (conidx: %u), idx: %u", conidx, idx);
		rc = ATT_ERR_REQUEST_NOT_SUPPORTED;
		break;
	}

	rc = gatt_srv_att_read_get_cfm(conidx, user_lid, token, rc, co_buf_data_len(p_buf), p_buf);
	if (rc != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to confirm value read (conidx: %u), error: %u", conidx, rc);
	}

	if (p_buf != NULL) {
		co_buf_release(p_buf);
	}
}

static uint16_t utils_process_smp_req(const void *p_data, uint16_t len)
{
	struct net_buf *nb;

	nb = smp_packet_alloc();
	if (!nb) {
		return ATT_ERR_INSUFF_RESOURCE;
	}

	if (net_buf_tailroom(nb) < len) {
		smp_packet_free(nb);
		return ATT_ERR_INSUFF_RESOURCE;
	}

	net_buf_add_mem(nb, p_data, len);
	smp_rx_req(&env.transport, nb);

	return GAP_ERR_NO_ERROR;
}

static uint16_t utils_process_ntf_cfq_req(const void *p_data, uint16_t len)
{
	uint16_t cfg;

	if (len != PRF_CCC_DESC_LEN) {
		return ATT_ERR_INVALID_ATTRIBUTE_VAL_LEN;
	}

	memcpy(&cfg, p_data, PRF_CCC_DESC_LEN);

	if (cfg != PRF_CLI_START_NTF && cfg != PRF_CLI_STOP_NTFIND) {
		return ATT_ERR_REQUEST_NOT_SUPPORTED;
	}

	env.ntf_cfg = cfg;

	return GAP_ERR_NO_ERROR;
}

static void on_cb_att_val_set(uint8_t conidx, uint8_t user_lid, uint16_t token, uint16_t hdl,
			      uint16_t offset, co_buf_t *p_data)
{
	uint16_t rc;
	uint16_t idx = hdl - env.start_hdl;

	switch (idx) {
	case SMP_GATT_ID_VAL:
		rc = utils_process_smp_req(co_buf_data(p_data), co_buf_data_len(p_data));
		if (rc != GAP_ERR_NO_ERROR) {
			LOG_ERR("Failed to process SMP request (conidx: %u), error: %u", conidx,
				rc);
			break;
		}

		LOG_INF("Received SMP request (conidx: %u)", conidx);
		break;

	case SMP_GATT_ID_NTF_CFG:
		rc = utils_process_ntf_cfq_req(co_buf_data(p_data), co_buf_data_len(p_data));
		if (rc != GAP_ERR_NO_ERROR) {
			LOG_ERR("Failed to process notification configuration (conidx: %u), error: "
				"%u",
				conidx, rc);
			break;
		}

		LOG_INF("Received notification configuration (conidx: %u), config: %u", conidx,
			env.ntf_cfg);
		break;

	default:
		LOG_ERR("Value set to unknown characteristic (conidx: %u), idx: %u", conidx, idx);
		rc = ATT_ERR_REQUEST_NOT_SUPPORTED;
		break;
	}

	rc = gatt_srv_att_val_set_cfm(conidx, user_lid, token, rc);
	if (rc != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to confirm value set (conidx: %u), error: %u", conidx, rc);
	}
}

static uint16_t utils_add_service(void)
{
	uint16_t rc;

	static const gatt_srv_cb_t gatt_cbs = {
		.cb_event_sent = on_cb_event_sent,
		.cb_att_read_get = on_cb_att_read_get,
		.cb_att_val_set = on_cb_att_val_set,
	};

	static const gatt_att_desc_t att_desc[] = {
		[SMP_GATT_ID_SERVICE] = {GATT_DECL_PRIMARY_SERVICE_UUID128, ATT_UUID(16) | PROP(RD),
					 0},

		[SMP_GATT_ID_CHAR] = {GATT_DECL_CHARACTERISTIC_UUID128, ATT_UUID(16) | PROP(RD), 0},

		[SMP_GATT_ID_VAL] = {SMP_CHARACTERISTIC_UUID128, ATT_UUID(128) | PROP(WC) | PROP(N),
				     CFG_ATT_VAL_MAX | OPT(NO_OFFSET)},

		[SMP_GATT_ID_NTF_CFG] = {GATT_DESC_CLIENT_CHAR_CFG_UUID128,
					 ATT_UUID(16) | PROP(RD) | PROP(WR),
					 PRF_CCC_DESC_LEN | OPT(NO_OFFSET)},
	};

	static const uint8_t service_uuid[] = SMP_SERVICE_UUID128;

	LOG_INF("Registering GATT server");

	rc = gatt_user_srv_register(CFG_MAX_LE_MTU, 0, &gatt_cbs, &env.user_lid);
	if (rc != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to register gatt server, error: %u", rc);
		return rc;
	}

	LOG_INF("Adding GATT service");

	rc = gatt_db_svc_add(env.user_lid, SVC_UUID(128), service_uuid, SMP_GATT_ID_END, NULL,
			     att_desc, SMP_GATT_ID_END, &env.start_hdl);
	if (rc != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to add gatt service, error: %u", rc);
		return rc;
	}

	LOG_INF("GATT service added, start_hdl: %u", env.start_hdl);

	return GAP_ERR_NO_ERROR;
}

static uint16_t utils_create_adv(void)
{
	static const gapm_le_adv_create_param_t adv_create_params = {
		.prop = GAPM_ADV_PROP_UNDIR_CONN_MASK,
		.disc_mode = GAPM_ADV_MODE_GEN_DISC,
#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0
		.tx_pwr = 0,
#else
		.max_tx_pwr = 0,
#endif /* CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 */
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

static void on_gapm_name_proc_cmp_cb(uint32_t metainfo, uint16_t status)
{
	uint16_t rc;

	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("GAPM name set callback failed, error: %u", status);
		return;
	}

	LOG_INF("Creating service");
	rc = utils_add_service();
	if (rc != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to add service, error: %u", rc);
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

	config_battery_service();
}
#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0
static void on_ctrl_hw_error(uint32_t metainfo, uint8_t code)
{
	LOG_ERR("hw_err_code: %u", code);
}
#else
static void on_ctrl_hw_error(enum co_error hw_err_code)
{
	LOG_ERR("hw_err_code: %u", hw_err_code);
}
#endif /* !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 */

static uint16_t utils_config_gapm(void)
{
	static gapm_config_t gapm_cfg = {
		.role = GAP_ROLE_LE_PERIPHERAL,
		.pairing_mode = GAPM_PAIRING_DISABLE,
		.pairing_min_req_key_size = 0,
		.privacy_cfg = GAPM_PRIV_CFG_PRIV_EN_BIT,
		.renew_dur = 1500,
		.private_identity.addr = {0xC0, 0x01, 0x23, 0x45, 0x67, 0x89},
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
#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0
	static const gapm_cb_t gapm_err_cbs = {
		.cb_hw_error = on_ctrl_hw_error,
	};

	static const gapm_callbacks_t gapm_cbs = {
		.p_con_req_cbs = &gapc_con_cbs,
		.p_sec_cbs = &gapc_sec_cbs,
		.p_info_cbs = &gapc_con_inf_cbs,
		.p_le_config_cbs = &gapc_le_cfg_cbs,
		.p_gapm_cbs = &gapm_err_cbs,
	};

#else
	static const gapm_err_info_config_cb_t gapm_err_cbs = {
		.ctrl_hw_error = on_ctrl_hw_error,
	};

	static const gapm_callbacks_t gapm_cbs = {
		.p_con_req_cbs = &gapc_con_cbs,
		.p_sec_cbs = &gapc_sec_cbs,
		.p_info_cbs = &gapc_con_inf_cbs,
		.p_le_config_cbs = &gapc_le_cfg_cbs,
		.p_err_info_config_cbs = &gapm_err_cbs,
	};
#endif /* CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 */

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

static uint16_t utils_get_mtu(void)
{
	uint16_t mtu;

	alif_ble_mutex_lock(K_FOREVER);
	mtu = gatt_bearer_mtu_min_get(env.conidx);
	alif_ble_mutex_unlock();

	return mtu - GATT_NTF_HEADER_LEN;
}

static uint16_t utils_send_ntf(const void *p_data, uint16_t len)
{
	uint16_t rc;
	co_buf_t *p_buf;

	k_sem_reset(&env.ntf_sem);

	alif_ble_mutex_lock(K_FOREVER);

	do {
		rc = co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, len, GATT_BUFFER_TAIL_LEN);
		if (rc != CO_BUF_ERR_NO_ERROR) {
			rc = GAP_ERR_INSUFF_RESOURCES;
			break;
		}

		memcpy(co_buf_data(p_buf), p_data, len);
		rc = gatt_srv_event_send(env.conidx, env.user_lid, 0, GATT_NOTIFY,
					 env.start_hdl + SMP_GATT_ID_VAL, p_buf);
		co_buf_release(p_buf);
	} while (false);

	alif_ble_mutex_unlock();

	if (rc == GAP_ERR_NO_ERROR) {
		k_sem_take(&env.ntf_sem, K_FOREVER);
	}

	return rc;
}

static int transport_out(struct net_buf *nb)
{
	int rc = MGMT_ERR_EOK;
	uint16_t off = 0;
	uint16_t mtu;
	uint16_t tx_size;
	uint16_t tx_rc;

	/* SMP response packet might be bigger than MTU, transmit response in MTU size chunks */

	mtu = utils_get_mtu();

	while (off < nb->len) {
		tx_size = MIN(nb->len - off, mtu);

		tx_rc = utils_send_ntf(&nb->data[off], tx_size);
		if (tx_rc != GAP_ERR_NO_ERROR) {
			LOG_ERR("Failed to send notification, error: %u", tx_rc);
			rc = MGMT_ERR_EUNKNOWN;
			break;
		}

		off += tx_size;
	}

	LOG_INF("Sent SMP response notification (conidx: %u)", env.conidx);

	smp_packet_free(nb);

	return rc;
}

static uint16_t transport_get_mtu(const struct net_buf *nb)
{
	ARG_UNUSED(nb);
	/* Seems that current SMP implementation does not call get_mtu at all, so output function
	 * needs to handle MTU by itself.
	 */
	return utils_get_mtu();
}

static bool transport_query_valid_check(struct net_buf *nb, void *arg)
{
	ARG_UNUSED(nb);
	ARG_UNUSED(arg);
	/* Mark all pending requests invalid when smp_rx_remove_invalid is called on disconnection.
	 */
	return false;
}

int main(void)
{
	int rc;

	LOG_INF("Alif smp_svr build time: " __DATE__ " " __TIME__);

	env.conidx = GAP_INVALID_CONIDX;
	env.adv_actv_idx = GAP_INVALID_ACTV_IDX;
	env.ntf_cfg = PRF_CLI_STOP_NTFIND;
	env.start_hdl = GATT_INVALID_HDL;
	env.user_lid = GATT_INVALID_USER_LID;
	k_sem_init(&env.ntf_sem, 0, 1);

	env.transport.functions.output = transport_out;
	env.transport.functions.get_mtu = transport_get_mtu;
	env.transport.functions.query_valid_check = transport_query_valid_check;

	rc = smp_transport_init(&env.transport);
	if (rc != 0) {
		LOG_ERR("Failed to init transport");
		return -1;
	}

	LOG_INF("Enabling Alif BLE stack");
	rc = alif_ble_enable(on_ble_enabled);
	if (rc) {
		LOG_ERR("Failed to enable Alif BLE stack, error: %i", rc);
		return -1;
	}

	/* Share connection info */
	service_conn(&ctrl);

	LOG_INF("Waiting for SMP requests...");
	while (1) {
		k_sleep(K_SECONDS(1));
		battery_process();
	}

	return 0;
}
