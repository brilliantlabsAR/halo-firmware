/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
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
#include "gapm_le_adv.h"
#include "co_buf.h"
#include "shared_control.h"

/*  Profiles definitions */
#include "prf.h"
#include "proxr.h"
#include "proxr_msg.h"
#include "prxp_app.h"

LOG_MODULE_REGISTER(prxp, LOG_LEVEL_DBG);

void ll_notify(void);
void ias_reset(void);

static uint8_t ll_level;
static uint8_t iass_level;

/* profile callbacks */
static void on_alert_upd(uint8_t conidx, uint8_t char_code, uint8_t alert_lvl)
{
	LOG_WRN("ALERT UPDATE");
	switch (char_code) {
	case PROXR_ERR_CHAR:
		LOG_DBG("PROXR_ERR_CHAR");
		break;
	case PROXR_LLS_CHAR:
		LOG_DBG("PROXR_LLS_CHAR");
		break;
	case PROXR_IAS_CHAR:
		LOG_DBG("PROXR_IAS_CHAR");
		break;
	default:
		LOG_DBG("alert char_code %02x", char_code);
		break;
	}

	switch (alert_lvl) {
	case PROXR_ALERT_NONE:
		LOG_DBG("PROXR_ALERT_NONE");
		break;
	case PROXR_ALERT_MILD:
		LOG_DBG("PROXR_ALERT_MILD");
		break;
	case PROXR_ALERT_HIGH:
		LOG_DBG("PROXR_ALERT_HIGH");
		break;
	default:
		LOG_DBG("alert level value %02x", alert_lvl);
		break;
	}

	if (char_code == PROXR_IAS_CHAR) {
		iass_level = alert_lvl;
	}
}

static const proxr_cb_t proxr_cb = {
	.cb_alert_upd = on_alert_upd,
};

/* Add profile to the stack */
void server_configure(void)
{
	uint16_t err;

	/* Dynamic allocation of service start handle*/
	uint16_t start_hdl = 0;

	/* Database configuration structure */
	struct proxr_db_cfg proxr_cfg = {
		.features = PROXR_IAS_TXPS_NOT_SUP,
	};
	err = prf_add_profile(TASK_ID_PROXR, 0, 0, &proxr_cfg, &proxr_cb, &start_hdl);

	if (err) {
		LOG_ERR("Error %u adding profile", err);
	}
}

void ll_notify(void)
{
	if (ll_level != PROXR_ALERT_NONE) {
		LOG_WRN("Link lost alert with level 0x%02x", ll_level);
		ll_level = PROXR_ALERT_NONE;
	}
}

void ias_reset(void)
{
	iass_level = PROXR_ALERT_NONE;
}

void disc_notify(uint16_t reason)
{
	if (reason != LL_ERR_REMOTE_USER_TERM_CON) {
		ll_notify();
	}
	ias_reset();
}

static void on_gapm_err(enum co_error err)
{
	LOG_ERR("gapm error %d", err);
}

static const gapm_err_info_config_cb_t gapm_err_cbs = {
	.ctrl_hw_error = on_gapm_err,
};

gapm_callbacks_t append_cbs(gapm_callbacks_t *gapm_append_cbs)
{
	gapm_callbacks_t cbs = *gapm_append_cbs;

	cbs.p_err_info_config_cbs = &gapm_err_cbs;

	return cbs;
}

void tx_power_read(void)
{
	/* No need to implement, required for newer BLE ROM versions */
}

void ias_process(void)
{
	/* IAS alert shall continue until disconnection or set to None*/
	if (iass_level == PROXR_ALERT_MILD) {
		LOG_WRN("IAS mild alert");
	} else if (iass_level == PROXR_ALERT_HIGH) {
		LOG_WRN("IAS high alert");
	}
}

gapm_le_adv_create_param_t append_adv_param(gapm_le_adv_create_param_t *adv_append_params)
{
	int8_t tx_pwr_param = 0;

	gapm_le_adv_create_param_t adv_params = *adv_append_params;

	adv_params.max_tx_pwr = tx_pwr_param;
	return adv_params;
}
