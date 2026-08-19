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
#include "alif_ble.h"
#include "gapm.h"
#include "gapm_le.h"
#include "gapc.h"
#include "gapc_le.h"
#include "gapc_sec.h"
#include "gap_le.h"
#include "mesh_node.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);
/* Print the function names that are not implemented */
#define EMPTY_FUNC() LOG_DBG("Not Implemented")

/**
 * Bluetooth stack configuration
 */

/* GAP manager configuration */
static const gapm_config_t gapm_cfg = {
	.role = GAP_ROLE_LE_OBSERVER | GAP_ROLE_LE_BROADCASTER,
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

static void on_gapm_err(enum co_error err)
{
	EMPTY_FUNC();
}

static const gapm_err_info_config_cb_t gapm_err_cbs = {
	.ctrl_hw_error = on_gapm_err,
};

static const gapm_callbacks_t gapm_cbs = {
	.p_con_req_cbs = NULL,
	.p_sec_cbs = NULL,
	.p_info_cbs = NULL,
	.p_le_config_cbs = NULL,
	.p_bt_config_cbs = NULL, /* BT classic so not required */
	.p_err_info_config_cbs = &gapm_err_cbs,
};

static void on_gapm_process_complete(uint32_t metainfo, uint16_t status)
{
	if (status) {
		LOG_ERR("gapm process completed with error %u", status);
		return;
	}

	LOG_DBG("gapm process completed successfully");

	/* Configure mesh node */
	if (mesh_node_configure()) {
		LOG_ERR("Setting up mesh node failed");
	}
}

int main(void)
{
	/* Start up bluetooth host stack */
	alif_ble_enable(NULL);

	/* After gapm_configure returns successfully, all other operations will be started from
	 * callbacks
	 */
	uint16_t err = gapm_configure(0, &gapm_cfg, &gapm_cbs, on_gapm_process_complete);

	if (err) {
		LOG_ERR("gapm_configure error %u", err);
		return -1;
	}

	return 0;
}
