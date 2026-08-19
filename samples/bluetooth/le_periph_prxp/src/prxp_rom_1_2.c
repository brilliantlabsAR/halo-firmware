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
#include "llss.h"
#include "iass.h"
#include "tpss.h"
#include "prxp_app.h"

LOG_MODULE_REGISTER(prxp, LOG_LEVEL_DBG);

void ll_notify(void);
void ias_reset(void);
static bool tx_read_cmp;

/* profile callbacks */
static uint8_t ll_level;
static uint8_t iass_level;
static int8_t tx_pwr_lvl;

void ll_notify(void)
{
	if (ll_level != LLS_ALERT_LEVEL_NONE) {
		LOG_WRN("Link lost alert with level 0x%02x", ll_level);
		ll_level = LLS_ALERT_LEVEL_NONE;
	}
}

void ias_reset(void)
{
	iass_level = IAS_ALERT_LEVEL_NONE;
}

static void on_get_level_req(uint8_t conidx, uint16_t token)
{
	co_buf_t *p_buf;

	prf_buf_alloc(&p_buf, LLS_ALERT_LEVEL_SIZE);
	*co_buf_data(p_buf) = ll_level;
	llss_get_level_cfm(conidx, token, p_buf);
	co_buf_release(p_buf);

	LOG_DBG("Level requested");
}

static void on_set_level_req(uint8_t conidx, uint16_t token, co_buf_t *p_buf)
{
	uint8_t level = *co_buf_data(p_buf);
	uint16_t status;

	if (level < LLS_ALERT_LEVEL_MAX) {
		ll_level = level;
		status = GAP_ERR_NO_ERROR;
		LOG_INF("Set level requested: %d", level);
	} else {
		status = ATT_ERR_VALUE_NOT_ALLOWED;
	}

	llss_set_level_cfm(conidx, status, token);
}

static const llss_cbs_t llss_cb = {
	.cb_get_level_req = on_get_level_req,
	.cb_set_level_req = on_set_level_req,
};

static void on_level(uint8_t conidx, co_buf_t *p_buf)
{
	uint8_t level = *co_buf_data(p_buf);

	if (level < IAS_ALERT_LEVEL_MAX) {
		iass_level = level;
	} else {
		LOG_ERR("Invalid Immediate Alert Level");
	}
}

static const iass_cbs_t iass_cb = {
	.cb_level = on_level,
};

uint16_t temp_token;

static void cmp_cb(uint8_t conidx, uint32_t metainfo, uint16_t status, uint8_t phy,
		   int8_t power_level, int8_t max_power_level)
{
	tx_pwr_lvl = power_level;
	tx_read_cmp = true;
	co_buf_t *p_buf;

	prf_buf_alloc(&p_buf, TPS_LEVEL_SIZE);
	*co_buf_data(p_buf) = (uint8_t)tx_pwr_lvl;
	tpss_level_cfm(conidx, temp_token, p_buf);
	co_buf_release(p_buf);
	/* Show Tx Power value in signed integer format */
	LOG_INF("Tx Power level 1M PHY sent:: %" PRId8 "\n", tx_pwr_lvl);
	temp_token = 0;
}

static void on_level_req(uint8_t conidx, uint16_t token)
{
	temp_token = token;
	gapc_le_get_local_tx_power_level(conidx, 0, GAPC_PHY_PWR_1MBPS_VALUE, cmp_cb);
}

static const tpss_cbs_t tpss_cb = {
	.cb_level_req = on_level_req,
};

/* Add profile to the stack */
void server_configure(void)
{
	uint16_t err;

	/* Dynamic allocation of service start handle*/
	uint16_t start_hdl = 0;

	alif_ble_mutex_lock(K_FOREVER);
	err = prf_add_profile(TASK_ID_LLSS, 0, 0, NULL, &llss_cb, &start_hdl);
	alif_ble_mutex_unlock();

	alif_ble_mutex_lock(K_FOREVER);
	err = prf_add_profile(TASK_ID_IASS, 0, 0, NULL, &iass_cb, &start_hdl);
	alif_ble_mutex_unlock();

	alif_ble_mutex_lock(K_FOREVER);
	err = prf_add_profile(TASK_ID_TPSS, 0, 0, NULL, &tpss_cb, &start_hdl);
	alif_ble_mutex_unlock();

	if (err) {
		LOG_ERR("Error %u adding profile", err);
	}
}

void on_gapm_err(uint32_t metainfo, uint8_t code)
{
	LOG_ERR("gapm error %d", code);
}
static const gapm_cb_t gapm_err_cbs = {
	.cb_hw_error = on_gapm_err,
};

gapm_callbacks_t append_cbs(gapm_callbacks_t *gapm_append_cbs)
{
	gapm_callbacks_t cbs = *gapm_append_cbs;

	cbs.p_gapm_cbs = &gapm_err_cbs;
	return cbs;
}

gapm_le_adv_create_param_t append_adv_param(gapm_le_adv_create_param_t *adv_append_params)
{
	int8_t tx_pwr_param = 0;
	gapm_le_adv_create_param_t adv_params = *adv_append_params;

	adv_params.tx_pwr = tx_pwr_param;
	return adv_params;
}

void ias_process(void)
{
	/* IAS alert shall continue until disconnection or set to None*/
	if (iass_level == IAS_ALERT_LEVEL_MILD) {
		LOG_WRN("IAS mild alert");
	} else if (iass_level == IAS_ALERT_LEVEL_HIGH) {
		LOG_WRN("IAS high alert");
	}
}

void disc_notify(uint16_t reason)
{
	if (reason != LL_ERR_REMOTE_USER_TERM_CON) {
		ll_notify();
	}
	ias_reset();
}
