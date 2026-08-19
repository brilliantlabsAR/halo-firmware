/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/__assert.h>
#include "alif_ble.h"
#include "gapm.h"
#include "gapm_le.h"
#include "gapc.h"
#include "gapc_le.h"
#include "gapc_sec.h"
#include "unicast_sink.h"
#include "storage.h"

#define APPEARANCE_EARBUDS 0x0941
#define APPEARANCE_HEADSET 0x0942

#if CONFIG_UNICAST_BIDIR
#define APPEARANCE APPEARANCE_HEADSET
#else
#define APPEARANCE APPEARANCE_EARBUDS
#endif

#if CONFIG_UNICAST_LOCATION_BOTH
#define IRK_VAL 0x8B
#elif CONFIG_UNICAST_LOCATION_LEFT
#define IRK_VAL 0x8A
#elif CONFIG_UNICAST_LOCATION_RIGHT
#define IRK_VAL 0x89
#else
#define IRK_VAL 0x88
#endif

LOG_MODULE_REGISTER(main, CONFIG_MAIN_LOG_LEVEL);

K_SEM_DEFINE(gapm_init_sem, 0, 1);

struct connection_status {
#if CONFIG_USE_DIRECT_ADVERTISING_WHEN_RESTART
	gap_bdaddr_t addr; /*!< Peer device address */
#endif
	uint8_t conidx; /*!< connection index */
};

static struct connection_status app_con_info = {
	.conidx = GAP_INVALID_CONIDX,
#if CONFIG_USE_DIRECT_ADVERTISING_WHEN_RESTART
	.addr.addr_type = 0xff,
#endif
};

#if CONFIG_USE_DIRECT_ADVERTISING_WHEN_RESTART
#define APP_CON_ADDR &app_con_info.addr
#else
#define APP_CON_ADDR NULL
#endif

struct app_con_bond_data {
	gapc_pairing_keys_t keys;
	gapc_bond_data_t bond_data;
};

static struct app_con_bond_data app_con_bond_data;

/* Load names from configuration file */
const char device_name[] = CONFIG_BLE_DEVICE_NAME;

static const gap_sec_key_t gapm_irk = {.key = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x08, 0x11,
					       0x22, 0x33, 0x44, 0x55, 0x66, 0x77, IRK_VAL}};

/* ---------------------------------------------------------------------------------------- */
/* Settings NVM storage handlers */

static int storage_load_bond_data(void)
{
	if (storage_init() < 0) {
		return -1;
	}

	storage_load(SETTINGS_NAME_KEYS, &app_con_bond_data.keys, sizeof(app_con_bond_data.keys));
	storage_load(SETTINGS_NAME_BOND_DATA, &app_con_bond_data.bond_data,
		     sizeof(app_con_bond_data.bond_data));
#if CONFIG_USE_DIRECT_ADVERTISING_WHEN_RESTART
	storage_load(SETTINGS_NAME_PEER, &app_con_info.addr, sizeof(app_con_info.addr));
#endif

	LOG_DBG("Settings loaded successfully");
	return 0;
}

/* ---------------------------------------------------------------------------------------- */
/* Bluetooth GAPM callbacks */

static void on_get_peer_version_cmp_cb(uint8_t const conidx, uint32_t const metainfo,
				       uint16_t const status, const gapc_version_t *const p_version)
{
	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Client %u Peer version fetch failed! err:%u", conidx, status);
		return;
	}
	LOG_INF("Client %u company_id:%u, lmp_subversion:%u, lmp_version:%u", conidx,
		p_version->company_id, p_version->lmp_subversion, p_version->lmp_version);
}

static void on_peer_features_cmp_cb(uint8_t const conidx, uint32_t const metainfo, uint16_t status,
				    const uint8_t *const p_features)
{
	if (status == GAP_ERR_NO_ERROR) {
		LOG_INF("Client %u features: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X", conidx,
			p_features[0], p_features[1], p_features[2], p_features[3], p_features[4],
			p_features[5], p_features[6], p_features[7]);
	} else {
		LOG_ERR("Client %u get peer features failed! status:%u", conidx, status);
	}

	status = gapc_get_peer_version(conidx, 0, on_get_peer_version_cmp_cb);
	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Client %u unable to get peer version! err:%u", conidx, status);
	}
}

static void connection_confirm_not_bonded(uint_fast8_t const conidx,
					  gapc_bond_data_t const *const p_bond_data)
{
	uint_fast16_t status;

	status = gapc_le_connection_cfm(conidx, 0, p_bond_data);
	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Client %u connection confirmation failed! err:%u", conidx, status);
	}
	status = gapc_le_get_peer_features(conidx, 0, on_peer_features_cmp_cb);
	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Client %u Unable to get peer features! err:%u", conidx, status);
	}
}

static void on_address_resolved_cb(uint16_t status, const gap_addr_t *const p_addr,
				   const gap_sec_key_t *const pirk)
{
	uint_fast8_t const conidx = app_con_info.conidx;
	/* Check whether the peer device is bonded */
	bool const resolved = (status == GAP_ERR_NO_ERROR);

	LOG_INF("Client %u address resolve ready! status:%u, %s peer device", conidx, status,
		resolved ? "KNOWN" : "UNKNOWN");

#if CONFIG_USE_DIRECT_ADVERTISING_WHEN_RESTART
	app_con_info.addr.addr_type = GAP_ADDR_RAND;
	memcpy(app_con_info.addr.addr, p_addr->addr, sizeof(app_con_info.addr.addr));
#endif

	if (resolved) {
		gapc_le_connection_cfm(conidx, 0, &app_con_bond_data.bond_data);
		return;
	}
	connection_confirm_not_bonded(conidx, NULL);
}

static void on_le_connection_req(uint8_t const conidx, uint32_t const metainfo,
				 uint8_t const actv_idx, uint8_t const role,
				 const gap_bdaddr_t *const p_peer_addr,
				 const gapc_le_con_param_t *const p_con_params,
				 uint8_t const clk_accuracy)
{
	bool const public = (p_peer_addr->addr_type == GAP_ADDR_PUBLIC);

	app_con_info.conidx = conidx;
#if CONFIG_USE_DIRECT_ADVERTISING_WHEN_RESTART
	memcpy(&app_con_info.addr, p_peer_addr, sizeof(app_con_info.addr));
#endif

	if (!public) {
		/* Number of IRKs */
		uint8_t const nb_irk = 1;
		/* Resolve Address */
		uint16_t const status = gapm_le_resolve_address(
			(gap_addr_t *)p_peer_addr->addr, nb_irk, &app_con_bond_data.keys.irk.key,
			on_address_resolved_cb);

		if (status == GAP_ERR_INVALID_PARAM) {
			/* Address not resolvable, just confirm the connection */
			connection_confirm_not_bonded(conidx, NULL);
		} else if (status != GAP_ERR_NO_ERROR) {
			LOG_ERR("Client %u Unable to start resolve address! err:%u", conidx,
				status);
		}
	} else {
		gapc_le_connection_cfm(conidx, 0, &app_con_bond_data.bond_data);
	}

	LOG_INF("Connection request. conidx:%u (actv_idx:%u), role %s", conidx, actv_idx,
		role ? "PERIPH" : "CENTRAL");

	LOG_DBG("  interval %fms, latency %u, supervision timeout %ums, clk_accuracy:%u",
		(p_con_params->interval * 1.25), p_con_params->latency,
		(uint32_t)p_con_params->sup_to * 10, clk_accuracy);

	LOG_DBG("  Peer address: %s %02X:%02X:%02X:%02X:%02X:%02X", public ? "Public" : "Private",
		p_peer_addr->addr[5], p_peer_addr->addr[4], p_peer_addr->addr[3],
		p_peer_addr->addr[2], p_peer_addr->addr[1], p_peer_addr->addr[0]);
}

static const struct gapc_connection_req_cb gapc_con_cbs = {
	.le_connection_req = on_le_connection_req,
};

static void on_gapc_le_encrypt_req(uint8_t const conidx, uint32_t const metainfo,
				   uint16_t const ediv, const gap_le_random_nb_t *const p_rand)
{
	uint16_t const status = gapc_le_encrypt_req_reply(
		conidx, true, &app_con_bond_data.keys.ltk.key, app_con_bond_data.keys.ltk.key_size);

	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Client %u LE encryption reply failed! err:%u", conidx, status);
		return;
	}
}

static void on_gapc_sec_auth_info(uint8_t const conidx, uint32_t const metainfo,
				  uint8_t const sec_lvl, bool const encrypted,
				  uint8_t const key_size)
{
	LOG_INF("Client %u Link security info. level:%u, encrypted:%s, key_size:%u", conidx,
		sec_lvl, (encrypted ? "TRUE" : "FALSE"), key_size);
}

static void on_gapc_pairing_succeed(uint8_t const conidx, uint32_t const metainfo,
				    uint8_t const pairing_level, bool const enc_key_present,
				    uint8_t const key_type)
{
	bool const bonded = gapc_is_bonded(conidx);

	LOG_INF("Client %u pairing SUCCEED. pairing_level:%u, bonded:%s", conidx, pairing_level,
		bonded ? "TRUE" : "FALSE");

	app_con_bond_data.bond_data.pairing_lvl = pairing_level;
	app_con_bond_data.bond_data.enc_key_present = enc_key_present;

	storage_save(SETTINGS_NAME_BOND_DATA, &app_con_bond_data.bond_data,
		     sizeof(app_con_bond_data.bond_data));
#if CONFIG_USE_DIRECT_ADVERTISING_WHEN_RESTART
	storage_save(SETTINGS_NAME_PEER, &app_con_info.addr, sizeof(app_con_info.addr));
#endif
}

static void on_gapc_pairing_failed(uint8_t const conidx, uint32_t const metainfo,
				   uint16_t const reason)
{
	LOG_ERR("Client %u pairing failed, reason: 0x%04X", conidx, reason);
	app_con_info.conidx = GAP_INVALID_CONIDX;
}

static void on_gapc_info_req(uint8_t const conidx, uint32_t const metainfo, uint8_t const exp_info)
{
	uint16_t err;

	switch (exp_info) {
	case GAPC_INFO_IRK: {
		/* IRK exchange if bonding enabled */
		err = gapc_le_pairing_provide_irk(conidx, &gapm_irk);
		if (err) {
			LOG_ERR("Client %u IRK provide failed. err: %u", conidx, err);
			break;
		}
		LOG_INF("Client %u IRK sent successful", conidx);
		break;
	}
	case GAPC_INFO_CSRK: {
		/* CSRK exchange if bonding enabled */
		err = gapc_pairing_provide_csrk(conidx, &app_con_bond_data.bond_data.local_csrk);
		if (err) {
			LOG_ERR("Client %u CSRK provide failed. err: %u", conidx, err);
			break;
		}
		LOG_INF("Client %u CSRK sent successful", conidx);
		break;
	}
	case GAPC_INFO_BT_PASSKEY:
	case GAPC_INFO_PASSKEY_DISPLAYED: {
		err = gapc_pairing_provide_passkey(conidx, true, 123456);
		if (err) {
			LOG_ERR("Client %u PASSKEY provide failed. err: %u", conidx, err);
			break;
		}
		LOG_INF("Client %u PASSKEY 123456 provided", conidx);
		break;
	}
	default:
		LOG_WRN("Client %u Unsupported info %u requested!", conidx, exp_info);
		break;
	}
}

static void on_gapc_pairing_req(uint8_t const conidx, uint32_t const metainfo,
				uint8_t const auth_level)
{
	LOG_DBG("Client %u pairing requested. auth_level:%u", conidx, auth_level);

	gapc_pairing_t pairing_info = {
		.auth = GAP_AUTH_REQ_NO_MITM_NO_BOND,
		.iocap = GAP_IO_CAP_NO_INPUT_NO_OUTPUT,
		.ikey_dist = GAP_KDIST_ENCKEY | GAP_KDIST_IDKEY,
		.key_size = GAP_KEY_LEN,
		.oob = GAP_OOB_AUTH_DATA_NOT_PRESENT,
		.rkey_dist = GAP_KDIST_ENCKEY | GAP_KDIST_IDKEY,
	};

	if (IS_ENABLED(CONFIG_BONDING_ALLOWED) && (auth_level & GAP_AUTH_BOND)) {
		pairing_info.auth = GAP_AUTH_REQ_SEC_CON_BOND;
		pairing_info.iocap = GAP_IO_CAP_DISPLAY_ONLY;
	} else if (auth_level & GAP_AUTH_SEC_CON) {
		pairing_info.auth = GAP_AUTH_SEC_CON;
	}

	uint16_t const status = gapc_le_pairing_accept(conidx, true, &pairing_info, 0);

	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Pairing accept failed! error: %u", status);
	}
}

static void on_gapc_sec_numeric_compare_req(uint8_t const conidx, uint32_t const metainfo,
					    uint32_t const value)
{
	LOG_DBG("Client %u pairing - numeric compare. value:%u", conidx, value);
	gapc_pairing_numeric_compare_rsp(conidx, true);
}

static void on_key_received(uint8_t conidx, uint32_t metainfo, const gapc_pairing_keys_t *p_keys)
{
	LOG_DBG("Client %u keys received: key_bf:%u, level:%u", conidx, p_keys->valid_key_bf,
		p_keys->pairing_lvl);

	gapc_pairing_keys_t *const p_appkeys = &app_con_bond_data.keys;
	uint8_t key_bits = GAP_KDIST_NONE;

	if (p_keys->valid_key_bf & GAP_KDIST_ENCKEY) {
		memcpy(&p_appkeys->ltk, &p_keys->ltk, sizeof(p_appkeys->ltk));
		key_bits |= GAP_KDIST_ENCKEY;
	}

	if (p_keys->valid_key_bf & GAP_KDIST_IDKEY) {
		memcpy(&p_appkeys->irk, &p_keys->irk, sizeof(p_appkeys->irk));
		key_bits |= GAP_KDIST_IDKEY;
	}

	if (p_keys->valid_key_bf & GAP_KDIST_SIGNKEY) {
		memcpy(&p_appkeys->csrk, &p_keys->csrk, sizeof(p_appkeys->csrk));
		key_bits |= GAP_KDIST_SIGNKEY;
	}

	p_appkeys->pairing_lvl = p_keys->pairing_lvl;
	p_appkeys->valid_key_bf = key_bits;

	storage_save(SETTINGS_NAME_KEYS, &app_con_bond_data.keys, sizeof(app_con_bond_data.keys));
}

static const gapc_security_cb_t gapc_sec_cbs = {
	.le_encrypt_req = on_gapc_le_encrypt_req,
	.auth_info = on_gapc_sec_auth_info,
	.pairing_succeed = on_gapc_pairing_succeed,
	.pairing_failed = on_gapc_pairing_failed,
	.info_req = on_gapc_info_req,
	.pairing_req = on_gapc_pairing_req,
	.numeric_compare_req = on_gapc_sec_numeric_compare_req,
	.key_received = on_key_received,
	/* Others are useless for LE peripheral device */
};

static void on_disconnection(uint8_t const conidx, uint32_t const metainfo, uint16_t const reason)
{
	LOG_INF("Client %u disconnected for reason %u", conidx, reason);

	app_con_info.conidx = GAP_INVALID_CONIDX;
	/* Restart advertising... */
	unicast_sink_adv_start(APP_CON_ADDR);
}

static void on_bond_data_updated(uint8_t const conidx, uint32_t const metainfo,
				 const gapc_bond_data_updated_t *const p_data)
{
	LOG_INF("Client %u bond data updated: gatt_start_hdl:%u, gatt_end_hdl:%u, "
		"svc_chg_hdl:%u, cli_info:%u, cli_feat:%u, srv_feat:%u",
		conidx, p_data->gatt_start_hdl, p_data->gatt_end_hdl, p_data->svc_chg_hdl,
		p_data->cli_info, p_data->cli_feat, p_data->srv_feat);
}

static void on_name_get(uint8_t const conidx, uint32_t const metainfo, uint16_t const token,
			uint16_t const offset, uint16_t const max_len)
{
	const size_t device_name_len = sizeof(device_name) - 1;
	const size_t short_len = (device_name_len > max_len ? max_len : device_name_len);

	gapc_le_get_name_cfm(conidx, token, GAP_ERR_NO_ERROR, device_name_len, short_len,
			     (const uint8_t *)device_name);
}

static void on_appearance_get(uint8_t const conidx, uint32_t const metainfo, uint16_t const token)
{
	gapc_le_get_appearance_cfm(conidx, token, GAP_ERR_NO_ERROR, APPEARANCE);
}

static const gapc_connection_info_cb_t gapc_inf_cbs = {
	.disconnected = on_disconnection,
	.bond_data_updated = on_bond_data_updated,
	.name_get = on_name_get,
	.appearance_get = on_appearance_get,
	/* Other callbacks in this struct are optional */
};

static const gapc_le_config_cb_t gapc_le_cfg_cbs = {0};

#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
static void on_gapm_err(uint32_t metainfo, uint8_t code)
{
	LOG_ERR("GAPM error %d", code);
}

static const gapm_cb_t gapm_err_cbs = {
	.cb_hw_error = on_gapm_err,
};

#else /* ROM version 1.0 */
static void on_gapm_err(enum co_error err)
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
	.p_info_cbs = &gapc_inf_cbs,
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

static void on_gapm_process_complete(uint32_t const metainfo, uint16_t const status)
{
	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("GAPM process completed with error %u", status);
		return;
	}

	switch (metainfo) {
	case 1:
		LOG_DBG("GAPM configured successfully");
		break;
	case 2:
		LOG_DBG("GAPM name set successfully");
		break;
	case 3:
		LOG_DBG("GAPM reset successfully");
		break;
	default:
		LOG_ERR("GAPM Unknown metadata!");
		return;
	}

	k_sem_give(&gapm_init_sem);
}

static gap_addr_t private_address;

static void on_gapm_le_random_addr_cb(uint16_t const status, const gap_addr_t *const p_addr)
{
	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("GAPM address generation error %u", status);
		return;
	}

	LOG_DBG("Generated address: %02X:%02X:%02X:%02X:%02X:%02X", p_addr->addr[5],
		p_addr->addr[4], p_addr->addr[3], p_addr->addr[2], p_addr->addr[1],
		p_addr->addr[0]);

	private_address = *p_addr;

	storage_save(SETTINGS_NAME_ADDR, &private_address, sizeof(private_address));

	k_sem_give(&gapm_init_sem);
}

static int ble_stack_configure(uint8_t const role)
{
	/* Bluetooth stack configuration*/
	gapm_config_t gapm_cfg = {
		.role = role,
		.pairing_mode = GAPM_PAIRING_SEC_CON,
		.privacy_cfg = 0,
		.renew_dur = 15 * 60, /* 15 minutes */
		.private_identity.addr = {0},
		.irk = gapm_irk,
		.gap_start_hdl = 0,
		.gatt_start_hdl = 0,
		.att_cfg = 0,
		.sugg_max_tx_octets = GAP_LE_MAX_OCTETS,
		/* Use the minimum transmission time to minimize latency */
		.sugg_max_tx_time = GAP_LE_MIN_TIME,
		.tx_pref_phy = GAP_PHY_LE_2MBPS,
		.rx_pref_phy = GAP_PHY_LE_2MBPS,
		.tx_path_comp = 0,
		.rx_path_comp = 0,
		/* BT classic - not used */
		.class_of_device = 0x200408,
		.dflt_link_policy = 0,
	};

	int err;

	storage_load(SETTINGS_NAME_ADDR, &private_address, sizeof(private_address));

	if (private_address.addr[5] == 0) {
		/* Configure GAPM to prepare address generation */
		err = gapm_configure(1, &gapm_cfg, &gapm_cbs, on_gapm_process_complete);
		if (err != GAP_ERR_NO_ERROR) {
			LOG_ERR("gapm_configure error %u", err);
			return -1;
		}
		if (k_sem_take(&gapm_init_sem, K_MSEC(1000)) != 0) {
			LOG_ERR("  FAIL! GAPM config timeout!");
			return -1;
		}

		/* Generate random static address */
		err = gapm_le_generate_random_addr(GAP_BD_ADDR_STATIC, on_gapm_le_random_addr_cb);
		if (err != GAP_ERR_NO_ERROR) {
			LOG_ERR("gapm_le_generate_random_addr error %u", err);
			return -1;
		}
		if (k_sem_take(&gapm_init_sem, K_MSEC(1000)) != 0) {
			LOG_ERR("  FAIL! GAPM random address timeout!");
			return -1;
		}

		/* Reset GAPM to set address */
		err = gapm_reset(3, on_gapm_process_complete);
		if (err != GAP_ERR_NO_ERROR) {
			LOG_ERR("gapm_reset error %u", err);
			return -1;
		}
		if (k_sem_take(&gapm_init_sem, K_MSEC(1000)) != 0) {
			LOG_ERR("  FAIL! GAPM reset timeout!");
			return -1;
		}
	}

	/* Reconfigure GAPM with generated address */
	gapm_cfg.privacy_cfg = GAPM_PRIV_CFG_PRIV_ADDR_BIT;
#if CONFIG_PRIVACY_ENABLED
	gapm_cfg.privacy_cfg |= GAPM_PRIV_CFG_PRIV_EN_BIT;
#endif
	gapm_cfg.private_identity = private_address;

	err = gapm_configure(1, &gapm_cfg, &gapm_cbs, on_gapm_process_complete);
	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("gapm_configure error %u", err);
		return -1;
	}
	if (k_sem_take(&gapm_init_sem, K_MSEC(1000)) != 0) {
		LOG_ERR("  FAIL! GAPM config timeout!");
		return -1;
	}

	LOG_INF("Set name: %s", device_name);
	err = gapm_set_name(2, strlen(device_name), (const uint8_t *)device_name,
			    on_gapm_process_complete);
	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("gapm_set_name error %u", err);
		return -1;
	}
	if (k_sem_take(&gapm_init_sem, K_MSEC(1000)) != 0) {
		LOG_ERR("  FAIL! GAPM name set timeout!");
		return -1;
	}

#if CONFIG_BONDING_ALLOWED
	/* Configure security level */
	gapm_le_configure_security_level(GAP_SEC1_SEC_CON_PAIR_ENC);
	LOG_INF("Security enabled for bonding");
#endif

	gapm_le_set_appearance(APPEARANCE);

	gap_bdaddr_t identity;

	gapm_get_identity(&identity);

	if (identity.addr_type == GAP_ADDR_PUBLIC) {
		LOG_DBG("Device address type: Public");
	} else {
		uint32_t const type = identity.addr[5] >> 6;

		switch (type) {
		case 0b00:
			LOG_DBG("Device address type: Random Private Non-Resolvable Address");
			break;
		case 0b01:
			LOG_DBG("Device address type: Random Private Resolvable Address");
			break;
		case 0b11:
			LOG_DBG("Device address type: Random Static Address format");
			break;
		case 0b10:
		default:
			LOG_WRN("Device address type: unknown");
			break;
		}
	}
	LOG_INF("Device address: %02X:%02X:%02X:%02X:%02X:%02X", identity.addr[5], identity.addr[4],
		identity.addr[3], identity.addr[2], identity.addr[1], identity.addr[0]);

	LOG_DBG("BLE init complete!");

	return 0;
}

/* ---------------------------------------------------------------------------------------- */
/* Application entry point */

int main(void)
{
	LOG_INF("Alif Unicast Acceptor app started");

	if (storage_load_bond_data() < 0) {
		return -1;
	}

	int ret = alif_ble_enable(NULL);

	if (ret) {
		LOG_ERR("Failed to enable bluetooth, err %d", ret);
		return ret;
	}

	LOG_DBG("BLE enabled");

	if (0 != ble_stack_configure(GAP_ROLE_LE_PERIPHERAL)) {
		return -1;
	}

	if (0 != unicast_sink_init()) {
		return -1;
	}

	if (0 != unicast_sink_adv_start(APP_CON_ADDR)) {
		return -1;
	}

	while (1) {
		/* Nothing to do here... just sleep to keep app running */
		k_sleep(K_SECONDS(5));
	}

	return 0;
}
