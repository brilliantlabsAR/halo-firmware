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
#include "prf.h"
#include "gapm.h"
#include "gap.h"
#include "shared_control.h"

#include "cgmp_common.h"
#include "cgms.h"
#include "cgms_msg.h"


LOG_MODULE_REGISTER(cgms, LOG_LEVEL_DBG);

K_SEM_DEFINE(conn_sem, 0, 1);

#define CGMS_RUN_TIME_HOURS (5)
#define TIME_OFFSET_MINS (20)

/* BLE definitions */
#define local_sec_level GAP_SEC1_AUTH_PAIR_ENC

struct shared_control *s_shared_ptr;

static bool READY_TO_SEND;

static cgm_status_t cgms_status;

static cgm_meas_value_t meas;

void on_gapm_err(enum co_error err)
{
	LOG_ERR("gapm error %d", err);
}

const gapm_err_info_config_cb_t gapm_err_cbs = {
	.ctrl_hw_error = on_gapm_err,
};

gapm_callbacks_t append_cbs(gapm_callbacks_t *gapm_append_cbs)
{
	gapm_callbacks_t cbs = *gapm_append_cbs;

	cbs.p_err_info_config_cbs = &gapm_err_cbs;

	return cbs;
}

/*
 * CGMS callbacks
 */

static void on_cgms_meas_send_complete(uint8_t conidx, uint16_t status)
{
	READY_TO_SEND = true;
}

static void on_bond_data_upd(uint8_t conidx, uint8_t char_code, uint16_t cfg_val)
{
	switch (cfg_val) {
	case PRF_CLI_STOP_NTFIND:
		LOG_INF("Client requested stop notification/indication (conidx: %u)", conidx);
		READY_TO_SEND = false;
		break;
	case PRF_CLI_START_NTF:
		LOG_INF("Client requested start notification/indication (conidx: %u)", conidx);
			READY_TO_SEND = true;
			LOG_INF("Sending measurements");
		break;

	case PRF_CLI_START_IND:
	default:
		break;
	}
}

static void on_rd_status_req(uint8_t conidx, uint32_t token)
{
	uint16_t status = 0;
	uint16_t err;

	/* Dummy offset */
	cgms_status.time_offset = meas.time_offset;

	err = cgms_rd_status_cfm(conidx, token, status, &(cgms_status));

	if (err) {
		LOG_ERR(" Error sending status 0x%04x", err);
	}
}

/* Dummy start time info for request */
cgm_sess_start_time_t p_start_time_data = {
	.date_time.year = 2025,
	.date_time.month = 10,
};

static void on_re_sess_start_time_req(uint8_t conidx, uint32_t token)
{
	uint16_t status = GAP_ERR_NO_ERROR;

	cgms_rd_sess_start_time_cfm(conidx, token, status,
		&p_start_time_data);
}

static void on_rd_sess_run_time_req(uint8_t conidx, uint32_t token)
{
	uint16_t status;

	status = GAP_ERR_NO_ERROR;
	cgms_rd_sess_run_time_cfm(conidx, token, status, CGMS_RUN_TIME_HOURS);
}

static void on_sess_start_time_upd(uint8_t conidx, const cgm_sess_start_time_t *p_sess_start_time)
{
	/* For the sample application, a single session is started during startup */
	LOG_DBG("Session start time update attempt");
}

static void on_racp_req(uint8_t conidx, uint8_t op_code, uint8_t func_operator, uint8_t filter_type,
			uint16_t min_time_offset, uint16_t max_time_offset)
{
	/* Command received on  the RACP control point */
	/* No records are stored for this sample application */
}

static void on_racp_rsp_send_cmp(uint8_t conidx, uint16_t status)
{
	/* Completion of RACP command response transmission */
	/* No records are stored for this sample application */
}

static void on_ops_ctrl_pt_req(uint8_t conidx, uint8_t op_code,
			       const union cgm_ops_operand *p_operand)
{
	/* Command received on  the RACP control point */
	/* Special OPS not supported by this sample application */
}

static void on_ops_ctrl_pt_rsp_send_cmp(uint8_t conidx, uint16_t status)
{
	/* Completion of Special OPS Control Point command response transmission */
	/* Special OPS not supported by this sample application */
}

static const cgms_cb_t cgms_cb = {
	.cb_meas_send_cmp = on_cgms_meas_send_complete,
	.cb_bond_data_upd = on_bond_data_upd,
	.cb_rd_status_req = on_rd_status_req,
	.cb_rd_sess_start_time_req = on_re_sess_start_time_req,
	.cb_rd_sess_run_time_req = on_rd_sess_run_time_req,
	.cb_sess_start_time_upd = on_sess_start_time_upd,
	.cb_racp_req = on_racp_req,
	.cb_racp_rsp_send_cmp = on_racp_rsp_send_cmp,
	.cb_ops_ctrl_pt_req = on_ops_ctrl_pt_req,
	.cb_ops_ctrl_pt_rsp_send_cmp = on_ops_ctrl_pt_rsp_send_cmp,
};

/* Add heart rate profile to the stack */
void server_configure(void)
{
	uint16_t err;
	uint16_t start_hdl = 0;
	struct cgms_db_cfg cgms_cfg;

	cgms_cfg.cgm_feature = CGM_FEAT_HYPO_ALERT_SUP_BIT | CGM_FEAT_SENSOR_MALFUNC_DETEC_SUP_BIT;
	cgms_cfg.type_sample = CGM_TYPE_SMP_CAPILLARY_WHOLE_BLOOD;
	cgms_cfg.sample_location = CGM_SMP_LOC_FINGER;

	err = prf_add_profile(TASK_ID_CGMS, local_sec_level, 0, &cgms_cfg, &cgms_cb, &start_hdl);

	if (err) {
		LOG_ERR("Error %u adding profile", err);
	}
}

/*  Generate and send dummy data*/
static void send_measurement(uint16_t current_value)
{
	uint16_t err;
	/* Dummy measurement data */
	meas.flags = CGM_MEAS_FLAGS_CGM_TREND_INFO_BIT | CGM_MEAS_FLAGS_CGM_QUALITY_BIT;
	meas.gluc_concent = current_value - 20;
	meas.time_offset = current_value - 69;
	meas.warn = 0;
	meas.cal_temp = 0;
	meas.sensor_status = CGM_MEAS_ANNUNC_STATUS_DEV_BATT_LOW_BIT;
	meas.trend_info = current_value - 50;

	/* Send measurement to connected device */
	/* Set 0 to first parameter to send only to the first connected peer device */
	err = cgms_meas_send(0, &meas);

	if (err) {
		LOG_ERR("Error %u sending measurement", err);
	}
}

void cgms_process(uint16_t measurement)
{
	if (s_shared_ptr->connected && READY_TO_SEND) {
		send_measurement(measurement);
		READY_TO_SEND = false;
	} else if (!s_shared_ptr->connected) {
		LOG_DBG("Waiting for peer connection...\n");
		k_sem_take(&conn_sem, K_FOREVER);
	}
}

void addr_res_done(void)
{
	/* Continue app */
	k_sem_give(&conn_sem);
}

void service_conn_cgms(struct shared_control *ctrl)
{
	s_shared_ptr = ctrl;
}

void disc_notify(uint16_t reason)
{
	READY_TO_SEND = false;
}
