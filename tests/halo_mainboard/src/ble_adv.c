/*
 * Copyright (c) 2026 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal BLE advertiser for the station's radio check: the DUT advertises a
 * per-unit name from its static identity address, and the station scans for
 * it and checks RSSI. Nothing is served over GATT. Flow follows Alif's
 * le_periph_hello sample (configure -> create -> set data -> start).
 */

#include <string.h>
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

#include "ble_adv.h"

LOG_MODULE_REGISTER(mbt_ble, LOG_LEVEL_WRN);

static K_SEM_DEFINE(step_sem, 0, 1);
static volatile uint16_t step_status;
static char adv_name[32];
static volatile bool adv_running;

/* The stack runs in its own thread; any API call made from another thread
 * must hold its mutex (alif_ble.h). Callbacks already run under it. */
#define BLE_CALL(expr)                                                                             \
	({                                                                                         \
		alif_ble_mutex_lock(K_FOREVER);                                                    \
		uint16_t _err = (expr);                                                            \
		alif_ble_mutex_unlock();                                                           \
		_err;                                                                              \
	})

static void step_done(uint16_t status)
{
	step_status = status;
	k_sem_give(&step_sem);
}

static int step_wait(k_timeout_t timeout, const char *what)
{
	if (k_sem_take(&step_sem, timeout) != 0) {
		LOG_ERR("%s: timed out", what);
		return -ETIMEDOUT;
	}
	if (step_status != GAP_ERR_NO_ERROR) {
		LOG_ERR("%s: status %u", what, step_status);
		return -EIO;
	}
	return 0;
}

/* The station only scans; accept a stray connection rather than leave the
 * peer hanging, and keep advertising after it goes. */
static uint8_t adv_idx;

static void on_le_connection_req(uint8_t conidx, uint32_t metainfo, uint8_t actv_idx,
				 uint8_t role, const gap_bdaddr_t *p_peer_addr,
				 const gapc_le_con_param_t *p_con_params, uint8_t clk_accuracy)
{
	gapc_le_connection_cfm(conidx, 0, NULL);
}

static void on_disconnection(uint8_t conidx, uint32_t metainfo, uint16_t reason)
{
	gapm_le_adv_param_t params = {.duration = 0};

	gapm_le_start_adv(adv_idx, &params);
}

static void on_key_received(uint8_t conidx, uint32_t metainfo, const gapc_pairing_keys_t *p_keys)
{
}

static void on_name_get(uint8_t conidx, uint32_t metainfo, uint16_t token, uint16_t offset,
			uint16_t max_len)
{
	const size_t len = strlen(adv_name);

	gapc_le_get_name_cfm(conidx, token, GAP_ERR_NO_ERROR, len, MIN(len, max_len),
			     (const uint8_t *)adv_name);
}

static void on_appearance_get(uint8_t conidx, uint32_t metainfo, uint16_t token)
{
	gapc_le_get_appearance_cfm(conidx, token, GAP_ERR_NO_ERROR, 0);
}

static const gapc_connection_req_cb_t con_cbs = {
	.le_connection_req = on_le_connection_req,
};

static const gapc_security_cb_t sec_cbs = {
	.key_received = on_key_received,
};

static const gapc_connection_info_cb_t info_cbs = {
	.disconnected = on_disconnection,
	.name_get = on_name_get,
	.appearance_get = on_appearance_get,
};

static const gapc_le_config_cb_t le_cfg_cbs;

#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0
static void on_gapm_err(uint32_t metainfo, uint8_t code)
{
	LOG_ERR("gapm error %d", code);
}
static const gapm_cb_t gapm_err_cbs = {
	.cb_hw_error = on_gapm_err,
};
static const gapm_callbacks_t gapm_cbs = {
	.p_con_req_cbs = &con_cbs,
	.p_sec_cbs = &sec_cbs,
	.p_info_cbs = &info_cbs,
	.p_le_config_cbs = &le_cfg_cbs,
	.p_bt_config_cbs = NULL,
	.p_gapm_cbs = &gapm_err_cbs,
};
#else
static void on_gapm_err(enum co_error err)
{
	LOG_ERR("gapm error %d", err);
}
static const gapm_err_info_config_cb_t gapm_err_cbs = {
	.ctrl_hw_error = on_gapm_err,
};
static const gapm_callbacks_t gapm_cbs = {
	.p_con_req_cbs = &con_cbs,
	.p_sec_cbs = &sec_cbs,
	.p_info_cbs = &info_cbs,
	.p_le_config_cbs = &le_cfg_cbs,
	.p_bt_config_cbs = NULL,
	.p_err_info_config_cbs = &gapm_err_cbs,
};
#endif

static void on_gapm_done(uint32_t metainfo, uint16_t status)
{
	step_done(status);
}

static void on_adv_stopped(uint32_t metainfo, uint8_t actv_idx, uint16_t reason)
{
	adv_running = false;
}

static void on_adv_proc_cmp(uint32_t metainfo, uint8_t proc_id, uint8_t actv_idx, uint16_t status)
{
	if (proc_id == GAPM_ACTV_CREATE_LE_ADV) {
		adv_idx = actv_idx;
	} else if (proc_id == GAPM_ACTV_START && status == GAP_ERR_NO_ERROR) {
		adv_running = true;
	}
	step_done(status);
}

static void on_adv_created(uint32_t metainfo, uint8_t actv_idx, int8_t tx_pwr)
{
}

static const gapm_le_adv_cb_actv_t adv_cbs = {
	.hdr.actv.stopped = on_adv_stopped,
	.hdr.actv.proc_cmp = on_adv_proc_cmp,
	.created = on_adv_created,
};

static int set_adv_data(void)
{
	const size_t name_len = strlen(adv_name);
	co_buf_t *buf;
	uint8_t *p;

	if (co_buf_alloc(&buf, 0, name_len + 2, 0) != 0) {
		return -ENOMEM;
	}
	p = co_buf_data(buf);
	p[0] = name_len + 1;
	p[1] = GAP_AD_TYPE_COMPLETE_NAME;
	memcpy(p + 2, adv_name, name_len);

	uint16_t err = BLE_CALL(gapm_le_set_adv_data(adv_idx, buf));

	co_buf_release(buf);
	return err ? -EIO : 0;
}

static int set_scan_rsp_data(void)
{
	co_buf_t *buf;

	if (co_buf_alloc(&buf, 0, 0, 0) != 0) {
		return -ENOMEM;
	}

	uint16_t err = BLE_CALL(gapm_le_set_scan_response_data(adv_idx, buf));

	co_buf_release(buf);
	return err ? -EIO : 0;
}

int mbt_ble_adv_start(const char *name, const uint8_t addr[6], k_timeout_t step_timeout)
{
	static gapm_config_t cfg = {
		.role = GAP_ROLE_LE_PERIPHERAL,
		.pairing_mode = GAPM_PAIRING_DISABLE,
		/* Static random identity address, as the app uses */
		.privacy_cfg = GAPM_PRIV_CFG_PRIV_ADDR_BIT,
		.renew_dur = 1500,
		.sugg_max_tx_octets = GAP_LE_MAX_OCTETS,
		.sugg_max_tx_time = GAP_LE_MAX_TIME,
		.tx_pref_phy = GAP_PHY_LE_1MBPS,
		.rx_pref_phy = GAP_PHY_LE_1MBPS,
	};
	gapm_le_adv_create_param_t create = {
		.prop = GAPM_ADV_PROP_UNDIR_CONN_MASK,
		.disc_mode = GAPM_ADV_MODE_GEN_DISC,
#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0
		.tx_pwr = 0,
#else
		.max_tx_pwr = 0,
#endif
		.filter_pol = GAPM_ADV_ALLOW_SCAN_ANY_CON_ANY,
		.prim_cfg = {
			/* 100 ms: several packets per station scan window */
			.adv_intv_min = 160,
			.adv_intv_max = 160,
			.ch_map = ADV_ALL_CHNLS_EN,
			.phy = GAPM_PHY_TYPE_LE_1M,
		},
	};
	gapm_le_adv_param_t start = {.duration = 0};
	int ret;

	strncpy(adv_name, name, sizeof(adv_name) - 1);

	/* gapm wants the address least-significant byte first */
	for (int i = 0; i < 6; i++) {
		cfg.private_identity.addr[i] = addr[5 - i];
	}

	ret = alif_ble_enable(NULL);
	if (ret) {
		LOG_ERR("alif_ble_enable: %d", ret);
		return ret;
	}

	if (BLE_CALL(gapm_configure(0, &cfg, &gapm_cbs, on_gapm_done)) != GAP_ERR_NO_ERROR) {
		return -EIO;
	}
	ret = step_wait(step_timeout, "configure");
	if (ret) {
		return ret;
	}

	if (BLE_CALL(gapm_le_create_adv_legacy(0, GAPM_STATIC_ADDR, &create, &adv_cbs)) != 0) {
		return -EIO;
	}
	ret = step_wait(step_timeout, "create adv");
	if (ret) {
		return ret;
	}

	ret = set_adv_data();
	if (ret == 0) {
		ret = step_wait(step_timeout, "adv data");
	}
	if (ret) {
		return ret;
	}

	ret = set_scan_rsp_data();
	if (ret == 0) {
		ret = step_wait(step_timeout, "scan rsp");
	}
	if (ret) {
		return ret;
	}

	if (BLE_CALL(gapm_le_start_adv(adv_idx, &start)) != 0) {
		return -EIO;
	}
	return step_wait(step_timeout, "start adv");
}

bool mbt_ble_adv_running(void)
{
	return adv_running;
}

static K_SEM_DEFINE(version_sem, 0, 1);
static gapm_version_t version;
static volatile uint16_t version_status;

static void on_version(uint32_t metainfo, uint16_t status, const gapm_version_t *p_version)
{
	version_status = status;
	if (status == GAP_ERR_NO_ERROR && p_version) {
		version = *p_version;
	}
	k_sem_give(&version_sem);
}

int mbt_ble_ping(k_timeout_t timeout, uint8_t *hci_ver, uint16_t *hci_subver)
{
	k_sem_reset(&version_sem);
	if (BLE_CALL(gapm_get_version(0, on_version)) != GAP_ERR_NO_ERROR) {
		return -EIO;
	}
	if (k_sem_take(&version_sem, timeout) != 0) {
		return -ETIMEDOUT;
	}
	if (version_status != GAP_ERR_NO_ERROR) {
		return -EIO;
	}
	*hci_ver = version.hci_ver;
	*hci_subver = version.hci_subver;
	return 0;
}
