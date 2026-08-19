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
#include "gapm.h"
#include "gap_le.h"
#include "gapc_le.h"
#include "gapc_sec.h"
#include "gapm_le.h"
#include "gapm_le_adv.h"
#include "co_buf.h"
#include "prf.h"
#include "bass.h"
#include "bas.h"
#include "batt_svc.h"
#include "shared_control.h"

static struct shared_control *s_shared_ptr = NULL;

LOG_MODULE_REGISTER(alif_batt, LOG_LEVEL_DBG);

#define BATT_INSTANCE 0x00

static uint8_t battery_level = 99;

#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
/* Notifications bit field */
uint16_t ccc_bf;
#else
static bool READY_TO_SEND_BASS;
#endif /* !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 */

#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */

__STATIC co_buf_t *battery_server_prepare_buf_level(void)
{
	co_buf_t *p_buf;

	prf_buf_alloc(&p_buf, BAS_LEVEL_SIZE_MAX);
	*co_buf_data(p_buf) = battery_level;

	return p_buf;
}

static void on_value_req(uint8_t conidx, uint8_t instance_idx, uint8_t char_type, uint16_t token)
{
	co_buf_t *p_buf = NULL;

	switch (char_type) {
	case BASS_CHAR_TYPE_LEVEL:
		p_buf = battery_server_prepare_buf_level();
		break;
	default:
		LOG_WRN("REQUEST NOT SUPPORTED");
		break;
	}

	if (p_buf != NULL) {
		bass_value_cfm(conidx, token, p_buf);
		co_buf_release(p_buf);
	}
}

static void on_get_cccd_req(uint8_t conidx, uint8_t instance_idx, uint8_t char_type, uint16_t token)
{
	co_buf_t *p_buf;
	uint16_t value = ((ccc_bf & CO_BIT(char_type)) != 0u)
				? PRF_CLI_START_NTF : PRF_CLI_STOP_NTFIND;

	prf_buf_alloc(&p_buf, PRF_CCC_DESC_LEN);
	co_write16(co_buf_data(p_buf), co_htole16(value));
	bass_value_cfm(conidx, token, p_buf);
	co_buf_release(p_buf);

	LOG_INF("Get CCCD request for 0x%02x characteristic",
		char_type);
}

static void on_set_cccd_req(uint8_t conidx, uint8_t instance_idx, uint8_t char_type, uint16_t token,
			co_buf_t *p_buf)
{
	LOG_INF("Set char_type: 0x%02x", char_type);

	uint16_t value = co_letoh16(co_read16(co_buf_data(p_buf)));

	if (value != PRF_CLI_STOP_NTFIND) {
		LOG_INF("Enable CCCD request for %02x characteristic (0x%04X)", char_type, value);

		ccc_bf |= CO_BIT(char_type);
	} else {
		LOG_INF("Disable CCCD request for %02x characteristic", char_type);
		ccc_bf &= ~CO_BIT(char_type);
	}

	bass_set_cccd_cfm(conidx, GAP_ERR_NO_ERROR, token);
}

static void on_sent(uint8_t conidx, uint8_t instance_idx, uint8_t char_type, uint16_t status)
{
	if (status) {
		LOG_WRN("Value sent with status: 0x%02x", status);
	}
}

static const bass_cbs_t bass_cb = {
	.cb_value_req = on_value_req,
	.cb_get_cccd_req = on_get_cccd_req,
	.cb_set_cccd_req = on_set_cccd_req,
	.cb_sent = on_sent,
};

uint16_t get_batt_id(void)
{
	return GATT_SVC_BATTERY;
}
#else

static void on_bass_batt_level_upd_cmp(uint16_t status)
{
	READY_TO_SEND_BASS = true;
}

static void on_bass_bond_data_upd(uint8_t conidx, uint8_t ntf_ind_cfg)
{
	switch (ntf_ind_cfg) {
	case PRF_CLI_STOP_NTFIND:
		LOG_INF("Client requested BASS stop notification/indication (conidx: %u)", conidx);
		READY_TO_SEND_BASS = false;
		break;

	case PRF_CLI_START_NTF:
	case PRF_CLI_START_IND:
		LOG_INF("Client requested BASS start notification/indication (conidx: %u)", conidx);
		READY_TO_SEND_BASS = true;
		LOG_DBG("Sending battery level");
		break;
	default:
		LOG_WRN("Unknown notification/indocation update");
		break;
	}
}

static const bass_cb_t bass_cb = {
	.cb_batt_level_upd_cmp = on_bass_batt_level_upd_cmp,
	.cb_bond_data_upd = on_bass_bond_data_upd,
};

uint16_t get_batt_id(void)
{
	return GATT_SVC_BATTERY_SERVICE;
}
#endif /* !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 */

void config_battery_service(void)
{
	uint16_t err;
	uint16_t start_hdl = 0;

	#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
	uint8_t bass_cfg_bf = 0;

	err = prf_add_profile(TASK_ID_BASS, 0, 0, &bass_cfg_bf, &bass_cb, &start_hdl);

	#else
	struct bass_db_cfg bass_cfg;

	bass_cfg.bas_nb = 1;
	bass_cfg.features[0] = 1;
	err = prf_add_profile(TASK_ID_BASS, 0, 0, &bass_cfg, &bass_cb, &start_hdl);
	#endif /* !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 */

	if (err) {
		LOG_ERR("Error adding service: 0x%02x", err);
	}
}


void battery_process(void)
{
	uint16_t err;

	/* Execute dummy measurement */	
	if (battery_level <= 1) {
		battery_level = 99;
	} else {
		battery_level--;
	}

	/* Check if connection is available */
	if (!s_shared_ptr->connected) {
		#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
		ccc_bf = 0;
		#else
		READY_TO_SEND_BASS = 0;
		#endif /* !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 */
		return;
	}

	/* Proceed to send measurement */
	#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
	if ((ccc_bf & CO_BIT(BASS_CHAR_TYPE_LEVEL)) != 0u) {
		co_buf_t *p_buf;
		uint8_t evt_type;

		evt_type = GATT_NOTIFY;
		p_buf = battery_server_prepare_buf_level();

		/* Sending dummy battery level to first battery instance*/
		err = bass_update_value(0, BATT_INSTANCE, BASS_CHAR_TYPE_LEVEL, evt_type, p_buf);

		if (err) {
			LOG_ERR("Error %u sending battery level", err);
		}

		co_buf_release(p_buf);
	}
	#else
	if (READY_TO_SEND_BASS) {
		/* Sending dummy battery level to first battery instance*/
		err = bass_batt_level_upd(BATT_INSTANCE, battery_level);

		if (err) {
			LOG_ERR("Error %u sending battery level", err);
		}
	}
	#endif /* !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 */
}

void service_conn(struct shared_control *ctrl)
{
	s_shared_ptr = ctrl;
}

