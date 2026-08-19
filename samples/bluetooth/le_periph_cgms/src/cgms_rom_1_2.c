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
#include "prf_types.h"
#include "cgmss.h"
#include "shared_control.h"
#include "co_time.h"

LOG_MODULE_REGISTER(cgms, LOG_LEVEL_DBG);

K_SEM_DEFINE(conn_sem, 0, 1);

#define CGMS_RUN_TIME_HOURS (5)
#define CGMS_CGM_TYPE CGMS_TYPE_CAPILLARY_WHOLE_BLOOD
#define CGMS_SAMPLE_LOCATION CGMS_SAMPLE_LOCATION_FINGER

typedef struct {
	prf_date_time_t date_time;
	int8_t time_zone;
	uint8_t dst_offset;
} app_start_time_t;

typedef struct {
	uint8_t cccd_state_bf;
} app_bond_data_t;

typedef struct {
	app_bond_data_t bond_data;
	app_start_time_t start_time;
	bool ready_to_send;
} app_env_t;

/* Dummy starting values for glucose and time offset */
uint16_t glucose = 0x00AA;
uint16_t time_offset_minutes = 0x00BB;

/* BLE definitions */
#define local_sec_level GAP_SEC1_AUTH_PAIR_ENC

struct shared_control *s_shared_ptr;

static void on_gapm_err(uint32_t metainfo, uint8_t code)
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

const char cgms_char_name[CGMS_CHAR_TYPE_MAX][31] = {
	"CGM Measurement",
	"CGM Feature",
	"Record Access Control Point",
	"CGM Specific Ops Control Point",
	"CGM Status",
	"CGM Session Start Time",
	"CGM Session Run Time",
};

static app_env_t app_env;

/*
 * CGMS callbacks
 */
static void on_set_session_start_time_req(uint8_t conidx, uint16_t token, co_buf_t *p_buf)
{
	/* for the sample application, a session is started during startup */
	uint16_t status = PRF_ERR_REQ_DISALLOWED;

	LOG_DBG("Sample application continuously running a sesison");
	cgmss_set_value_cfm(conidx, status, token);
}

static void on_value_req(uint8_t conidx, uint8_t char_type, uint16_t token)
{
	co_buf_t *p_buf;
	uint8_t *p_data;
	uint16_t length;

	switch (char_type) {
	case CGMS_CHAR_TYPE_FEATURE:
		length = CGMS_FEATURE_LEN;
		break;

	case CGMS_CHAR_TYPE_STATUS:
		length = CGMS_STATUS_LEN;
		break;

	case CGMS_CHAR_TYPE_SESSION_START_TIME:
		length = CGMS_SESSION_START_TIME_LEN;
		break;

	default:
		length = CGMS_SESSION_RUN_TIME_LEN;
		break;
	}

	cgms_buf_alloc(&p_buf, length);
	p_data = co_buf_data(p_buf);

	switch (char_type) {
	case CGMS_CHAR_TYPE_FEATURE:
		/* CGM Feature field */
		*p_data++ = 0u;

		/* CGMSS_E2E_CRC */
		*p_data++ = 0u;
		*p_data++ = 0u;

		/* CGM Type-Sample Location field */
		*p_data = CGMS_CGM_TYPE
			| (CGMS_SAMPLE_LOCATION << 4);
		break;

	case CGMS_CHAR_TYPE_STATUS:
		/* Time offset field */
		co_write16(p_data, co_htole16(time_offset_minutes));
		p_data += 2u;
		/* CGM Status field */
		*p_data++ = 0u;
		*p_data++ = 0u;
		*p_data = 0u;
		break;

	case CGMS_CHAR_TYPE_SESSION_START_TIME:
		/* Session Start Time field */
		co_write16(p_data, co_htole16(app_env.start_time.date_time.year));
		p_data += 2u;
		*p_data++ = app_env.start_time.date_time.month;
		*p_data++ = app_env.start_time.date_time.day;
		*p_data++ = app_env.start_time.date_time.hour;
		*p_data++ = app_env.start_time.date_time.min;
		*p_data++ = app_env.start_time.date_time.sec;
		/* Time Zone field */
		*p_data++ = app_env.start_time.time_zone;
		/* DST Offset field */
		*p_data = app_env.start_time.dst_offset;
		break;

	default:
		uint16_t time = CGMS_RUN_TIME_HOURS;

		co_write16(p_data, co_htole16(time));
		break;
	}

	cgmss_value_cfm(conidx, token, char_type, p_buf);
	co_buf_release(p_buf);

	LOG_DBG("Read request for %s characteristic", cgms_char_name[char_type]);
}

static void on_control_req(uint8_t conidx, uint8_t char_type, uint16_t token, co_buf_t *p_buf)
{
	/* No records stored for this sample application */
	uint16_t status = PRF_ERR_REQ_DISALLOWED;

	LOG_DBG("No records available");
	cgmss_set_value_cfm(conidx, status, token);
}

static void on_get_cccd_req(uint8_t conidx, uint8_t char_type, uint16_t token)
{
	co_buf_t *p_buf;
	uint16_t value;

	value = PRF_CLI_STOP_NTFIND;

	if ((app_env.bond_data.cccd_state_bf & CO_BIT(char_type)) != 0) {
		value = (char_type == CGMS_CHAR_TYPE_MEASUREMENT) ? PRF_CLI_START_NTF
		: PRF_CLI_START_IND;
	}

	cgms_buf_alloc(&p_buf, PRF_CCC_DESC_LEN);
	co_write16(co_buf_data(p_buf), co_htole16(value));
	cgmss_get_cccd_cfm(conidx, token, p_buf);
	co_buf_release(p_buf);

	LOG_DBG("Get CCCD request for %s characteristic", cgms_char_name[char_type]);
}

static void on_set_cccd_req(uint8_t conidx, uint8_t char_type, uint16_t token, co_buf_t *p_buf)
{
	uint16_t status = GAP_ERR_NO_ERROR;
	uint16_t value;

	value = co_letoh16(co_read16(co_buf_data(p_buf)));

	if (value != PRF_CLI_STOP_NTFIND) {
		if (((char_type == CGMS_CHAR_TYPE_MEASUREMENT) && (value == PRF_CLI_START_NTF))
			|| ((char_type != CGMS_CHAR_TYPE_MEASUREMENT)
			&& (value == PRF_CLI_START_IND))) {
			app_env.bond_data.cccd_state_bf |= CO_BIT(char_type);
			app_env.ready_to_send = true;
		} else {
			status = ATT_ERR_VALUE_NOT_ALLOWED;
		}
	} else {
		app_env.bond_data.cccd_state_bf &= ~CO_BIT(char_type);
		app_env.ready_to_send = false;
	}
	cgmss_set_value_cfm(conidx, status, token);
}

static void on_sent(uint8_t conidx, uint8_t char_type, uint16_t status)
{
	app_env.ready_to_send = true;
}

static const cgmss_cbs_t cgms_cb = {
	.cb_set_session_start_time_req = on_set_session_start_time_req,
	.cb_value_req = on_value_req,
	.cb_control_req = on_control_req,
	.cb_get_cccd_req = on_get_cccd_req,
	.cb_set_cccd_req = on_set_cccd_req,
	.cb_sent = on_sent,
};

static void set_start_time(void)
{
	/* dummy session start date and time */
	app_env.start_time.date_time.year = 2025;
	app_env.start_time.date_time.month = 1;
	app_env.start_time.date_time.day = 1;
	app_env.start_time.date_time.hour = 0;
	app_env.start_time.date_time.min = 0;
	app_env.start_time.date_time.sec = 0;
	app_env.start_time.time_zone = 10;
	app_env.start_time.dst_offset = 0;
}

void server_configure(void)
{
	uint16_t err;
	uint16_t start_hdl = 0;
	uint8_t cgmss_cfg_bf = 0;

	err = prf_add_profile(TASK_ID_CGMSS, local_sec_level, 0, &cgmss_cfg_bf,
			&cgms_cb, &start_hdl);

	if (err) {
		LOG_ERR("Error %u adding profile", err);
	}

	/* Set sample start time for CGMS session */
	set_start_time();
}

/*  Generate and send dummy data*/
static void send_measurement(uint16_t current_value)
{
	uint16_t err;

	co_buf_t *p_buf;
	uint8_t *p_data;

	cgms_buf_alloc(&p_buf, CGMS_MEASUREMENT_MIN_LEN);
	p_data = co_buf_data(p_buf);

	/* Size field */
	*p_data++ = CGMS_MEASUREMENT_MIN_LEN;
	/* Flags field */
	*p_data++ = 0u;
	/* CGM Glucose Concentration field */
	co_write16(p_data, co_htole16(glucose));
	p_data += 2;
	/* Time Offset field */
	co_write16(p_data, co_htole16(time_offset_minutes));

	err = cgmss_send_measurement(0, p_buf);
	co_buf_release(p_buf);

	if (err) {
		LOG_ERR("Error %u sending measurement", err);
	}
}

void cgms_process(uint16_t measurement)
{
	/* Dummy measurement data */
	glucose = (glucose >= 0x00DD ? 0x00A9 : glucose) + 1;
	time_offset_minutes = (time_offset_minutes >= 0x00EE ? 0x00BA : time_offset_minutes) + 1;

	if (s_shared_ptr->connected && app_env.ready_to_send) {
		send_measurement(measurement);
		app_env.ready_to_send = false;
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
	app_env.ready_to_send = false;
}
