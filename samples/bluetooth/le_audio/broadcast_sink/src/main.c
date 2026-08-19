/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include "alif_ble.h"
#include "gapm.h"
#include "gap_le.h"
#include "broadcast_sink.h"

LOG_MODULE_REGISTER(main, CONFIG_MAIN_LOG_LEVEL);

/**
 * Bluetooth stack configuration
 */
static const gapm_config_t gapm_cfg = {
	.role = GAP_ROLE_LE_OBSERVER,
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
	.class_of_device = 0,  /* BT Classic only */
	.dflt_link_policy = 0, /* BT Classic only */
};

#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
static void on_gapm_err(uint32_t metainfo, uint8_t code)
{
	LOG_ERR("gapm error %d", code);
}

static const gapm_cb_t gapm_err_cbs = {
	.cb_hw_error = on_gapm_err,
};

/* For the observer role, callbacks are not mandatory */
static const gapm_callbacks_t gapm_cbs = {
	.p_con_req_cbs = NULL,
	.p_sec_cbs = NULL,
	.p_info_cbs = NULL,
	.p_le_config_cbs = NULL,
	.p_bt_config_cbs = NULL,
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

/* For the observer role, callbacks are not mandatory */
static const gapm_callbacks_t gapm_cbs = {
	.p_con_req_cbs = NULL,
	.p_sec_cbs = NULL,
	.p_info_cbs = NULL,
	.p_le_config_cbs = NULL,
	.p_bt_config_cbs = NULL,
	.p_err_info_config_cbs = &gapm_err_cbs,
};
#endif

static void on_gapm_process_complete(uint32_t metainfo, uint16_t status)
{
	if (status) {
		LOG_ERR("gapm process completed with error %u", status);
		return;
	}

	LOG_INF("gapm process completed successfully");

	int ret = broadcast_sink_start();

	if (ret) {
		LOG_ERR("Failed to start broadcast sink, err %d", ret);
	}

	LOG_INF("Broadcast sink started");
}

int main(void)
{
	int ret = alif_ble_enable(NULL);

	if (ret) {
		LOG_ERR("Failed to enable bluetooth, err %d", ret);
		return ret;
	}

	LOG_INF("BLE enabled");

	uint16_t err = gapm_configure(0, &gapm_cfg, &gapm_cbs, on_gapm_process_complete);

	if (err != 0) {
		LOG_ERR("gapm_configure error %u", err);
		return -1;
	}

	return 0;
}
