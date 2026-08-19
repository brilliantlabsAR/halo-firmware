/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

/* This is a throughput central implementation
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
#include "gapm_le_init.h"
#include "gatt_db.h"

#include "common.h"
#include "config.h"
#include "peripheral.h"
#include "service_uuid.h"

LOG_MODULE_REGISTER(peripheral, LOG_LEVEL_INF);

#define ATT_16_TO_128_ARRAY(uuid)                                                                  \
	{(uuid) & 0xFF, (uuid >> 8) & 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}

/* Service Definitions */
#define ATT_128_PRIMARY_SERVICE ATT_16_TO_128_ARRAY(GATT_DECL_PRIMARY_SERVICE)
#define ATT_128_CHARACTERISTIC  ATT_16_TO_128_ARRAY(GATT_DECL_CHARACTERISTIC)
#define ATT_128_CLIENT_CHAR_CFG ATT_16_TO_128_ARRAY(GATT_DESC_CLIENT_CHAR_CFG)

/* List of attributes in the service */
enum service_att_list {
	LBS_IDX_SERVICE = 0,
	LBS_IDX_CHAR1_CHAR,
	LBS_IDX_CHAR1_VAL,
	LBS_IDX_CHAR1_NTF_CFG,
	/* Number of items*/
	LBS_IDX_NB,
};

/* GATT database for the service */
static const gatt_att_desc_t lbs_att_db[LBS_IDX_NB] = {
	[LBS_IDX_SERVICE] = {ATT_128_PRIMARY_SERVICE, ATT_UUID(16) | PROP(RD), 0},
	[LBS_IDX_CHAR1_CHAR] = {ATT_128_CHARACTERISTIC, ATT_UUID(16) | PROP(RD), 0},
	[LBS_IDX_CHAR1_VAL] = {LBS_UUID_16_CHAR1,
			       ATT_UUID(16) | PROP(WC) | PROP(RD) | PROP(N) | PROP(I),
			       CFG_ATT_VAL_MAX | OPT(NO_OFFSET)},
	[LBS_IDX_CHAR1_NTF_CFG] = {ATT_128_CLIENT_CHAR_CFG, ATT_UUID(16) | PROP(RD) | PROP(WR), 0},
};

#define LBS_METAINFO_CHAR0_NTF_SEND      0x1234
#define LBS_METAINFO_CHAR0_NTF_SEND_LAST 0x5678

/* Environment for the service */
static struct service_env {
	/* Accumulated reception time in microseconds */
	uint64_t accumulated_time_ns;
	/* Test duration (ms) */
	uint32_t test_duration_ms;
	/* Delay between data sends (ms) */
	uint32_t send_interval_ms;

	uint16_t start_hdl;
	uint8_t user_lid;
	uint8_t adv_actv_idx;
	struct tp_data resp_data;

	uint32_t start_time;
	uint16_t mtu;
	uint32_t total_len;
	uint16_t cnt;
} env;

static uint8_t service_uuid[] = SERVICE_UUID;

K_SEM_DEFINE(adv_started_sem, 0, 1);
K_SEM_DEFINE(app_sem, 0, 1);

/* ---------------------------------------------------------------------------------------- */
/* GATT SERVER CONFIG */

static void on_att_read_get(uint8_t const conidx, uint8_t const user_lid, uint16_t const token,
			    uint16_t const hdl, uint16_t const offset, uint16_t const max_length)
{
	ARG_UNUSED(offset);
	ARG_UNUSED(max_length);

	co_buf_t *p_buf = NULL;
	uint16_t status = GAP_ERR_NO_ERROR;
	uint16_t att_val_len = 0;
	uint8_t const att_idx = hdl - env.start_hdl;

	switch (att_idx) {
	case LBS_IDX_CHAR1_VAL: {
		printk("\r\n >>> RX done\r\n");

		att_val_len = sizeof(env.resp_data);

		status = co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, att_val_len,
				      GATT_BUFFER_TAIL_LEN);
		if (status != CO_BUF_ERR_NO_ERROR) {
			LOG_ERR("alloc error. Unable to send results!");
			p_buf = NULL;
			att_val_len = 0;
			status = ATT_ERR_APP_ERROR;
			app_transition_to(APP_STATE_ERROR);
			break;
		}

		if (env.accumulated_time_ns) {
			env.resp_data.write_rate =
				(((uint64_t)env.resp_data.write_len << 3) * 1000000000) /
				env.accumulated_time_ns;
		}

		memcpy(p_buf->buf + p_buf->head_len, &env.resp_data, att_val_len);
		if (IS_ENABLED(CONFIG_BLE_TP_BIDIRECTIONAL_TEST)) {
			app_transition_to(APP_STATE_PERIPHERAL_PREPARE_SENDING);
		} else {
			app_transition_to(APP_STATE_STANDBY);
		}
		break;
	}
	default: {
		status = ATT_ERR_INVALID_HANDLE;
		LOG_DBG("Read get undefined value %u", att_idx);
		break;
	}
	}

	/* Send the GATT response */
	gatt_srv_att_read_get_cfm(conidx, user_lid, token, status, att_val_len, p_buf);
	if (p_buf != NULL) {
		co_buf_release(p_buf);
	}
}

static int indication_send(void const *const p_data, size_t const size)
{
	static co_buf_t *p_buf;
	uint16_t status;

	status = co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, size, GATT_BUFFER_TAIL_LEN);
	if (status != CO_BUF_ERR_NO_ERROR) {
		LOG_ERR("Failed to allocate buffer");
		co_buf_release(p_buf);
		return -ENOMEM;
	}
	if (k_sem_take(&app_sem, K_MSEC(1000)) != 0) {
		LOG_ERR("Indication send error: failed to take semaphore");
		co_buf_release(p_buf);
		return -ENOEXEC;
	}

	memcpy(co_buf_data(p_buf), p_data, size);

	status = gatt_srv_event_send(0, env.user_lid, 0, GATT_INDICATE,
				     env.start_hdl + LBS_IDX_CHAR1_VAL, p_buf);
	co_buf_release(p_buf);

	return 0;
}

static uint16_t notification_send(void)
{
	uint16_t status = GAP_ERR_NO_ERROR;
	uint8_t conidx = 0;
	uint16_t metainfo = LBS_METAINFO_CHAR0_NTF_SEND;
	static co_buf_t *p_buf;

	size_t const data_len = env.mtu - 3;

	status = co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, data_len, GATT_BUFFER_TAIL_LEN);
	if (status != CO_BUF_ERR_NO_ERROR) {
		LOG_ERR("alloc error. Unable to send package!");
		app_transition_to(APP_STATE_ERROR);
		return GAP_ERR_INSUFF_RESOURCES;
	}

	env.total_len += data_len;
	env.cnt++;

	if ((int32_t)(k_uptime_get_32() - env.start_time) >= env.test_duration_ms) {
		metainfo = LBS_METAINFO_CHAR0_NTF_SEND_LAST;
		app_transition_to(APP_STATE_PERIPHERAL_SEND_RESULTS);
	}

	if (k_sem_take(&app_sem, K_MSEC(1000)) != 0) {
		co_buf_release(p_buf);
		return -1;
	}

	status = gatt_srv_event_send(conidx, env.user_lid, metainfo, GATT_NOTIFY,
				     env.start_hdl + LBS_IDX_CHAR1_VAL, p_buf);

	co_buf_release(p_buf);

	return status;
}

static void on_att_val_set(uint8_t const conidx, uint8_t const user_lid, uint16_t const token,
			   uint16_t const hdl, uint16_t const offset, co_buf_t *const p_data)
{
	ARG_UNUSED(offset);

	static uint32_t clock_cycles_last;

	uint32_t const cycle_now = k_cycle_get_32();
	uint16_t status = GAP_ERR_NO_ERROR;

	uint8_t const att_idx = hdl - env.start_hdl;

	switch (att_idx) {
	case LBS_IDX_CHAR1_VAL: {
		size_t const data_len = co_buf_data_len(p_data);

		/* Check if the control message was received */
		if (data_len == sizeof(struct tp_client_ctrl)) {
			struct tp_client_ctrl *p_ctrl =
				(struct tp_client_ctrl *)co_buf_data(p_data);

			if (p_ctrl->type == TP_CLIENT_CTRL_TYPE_RESET) {
				printk(" >>> Reception starts\r\n");

				env.test_duration_ms = p_ctrl->test_duration_ms;
				env.send_interval_ms = p_ctrl->send_interval_ms;

				env.accumulated_time_ns = 0;
				env.resp_data.write_count = 0;
				env.resp_data.write_len = 0;
				env.resp_data.write_rate = 0;

				clock_cycles_last = cycle_now;
				app_transition_to(APP_STATE_PERIPHERAL_RECEIVING);
				break;
			}
		}

		env.resp_data.write_len += data_len;
		env.resp_data.write_count++;

		env.accumulated_time_ns += k_cyc_to_ns_floor64(cycle_now - clock_cycles_last);
		clock_cycles_last = cycle_now;

		if ((env.resp_data.write_count % 256) == 0) {
			printk(".");
		}

		break;
	}
	case LBS_IDX_CHAR1_NTF_CFG: {
		app_transition_to(APP_STATE_PERIPHERAL_PREPARE_SENDING);
		break;
	}
	default:
		LOG_ERR("Request not supported");
		status = ATT_ERR_REQUEST_NOT_SUPPORTED;
		break;
	}

	status = gatt_srv_att_val_set_cfm(conidx, user_lid, token, status);
	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to confirm value set (conidx: %u), error: %u", conidx, status);
	}
}

static void on_event_sent(uint8_t const conidx, uint8_t const user_lid, uint16_t const metainfo,
			  uint16_t const status)
{
	ARG_UNUSED(conidx);
	ARG_UNUSED(user_lid);
	ARG_UNUSED(status);

	if (metainfo == LBS_METAINFO_CHAR0_NTF_SEND_LAST) {
		uint32_t const delta_ms = k_uptime_get_32() - env.start_time;

		env.resp_data.write_count = env.cnt;
		env.resp_data.write_len = env.total_len;
		env.resp_data.write_rate = (((uint64_t)env.total_len << 3) * 1000LLU) / delta_ms;

		printk("\r\n <<< TX done\r\n");
		LOG_DBG("Sending results to central");
	} else if ((env.cnt % 256) == 0) {
		printk(".");
	}

	k_sem_give(&app_sem);
}

/* ---------------------------------------------------------------------------------------- */
/* Service functions */

static uint16_t set_advertising_data(uint8_t const actv_idx)
{
	uint16_t err;

	/* Name advertising length */
	const char device_name[] = CONFIG_BLE_TP_DEVICE_NAME;

	const size_t device_name_len = sizeof(device_name) - 1;
	const uint16_t adv_device_name = GATT_HANDLE_LEN + device_name_len;

	/* gatt service identifier */
	const size_t uuid_len = sizeof(service_uuid);
	/* Service advertising length */
	const uint16_t adv_uuid_svc = GATT_HANDLE_LEN + uuid_len;

	/* Create advertising data with necessary services */
	const uint16_t adv_len = adv_device_name + adv_uuid_svc;

	co_buf_t *p_buf = NULL;
	uint8_t *p_data;

	err = co_buf_alloc(&p_buf, 0, adv_len, 0);
	if (err != GAP_ERR_NO_ERROR || !p_buf) {
		LOG_ERR("Buffer allocation failed");
		app_transition_to(APP_STATE_ERROR);
		return err;
	}

	p_data = co_buf_data(p_buf);

	/* Device name data */
	*p_data++ = device_name_len + 1;
	*p_data++ = GAP_AD_TYPE_COMPLETE_NAME;
	memcpy(p_data, device_name, device_name_len);

	/* Update data pointer */
	p_data += device_name_len;

	/* Service UUID data */
	*p_data++ = uuid_len + 1;
	switch (uuid_len) {
	case GATT_UUID_128_LEN:
		*p_data++ = GAP_AD_TYPE_COMPLETE_LIST_128_BIT_UUID;
		break;
	case GATT_UUID_32_LEN:
		*p_data++ = GAP_AD_TYPE_COMPLETE_LIST_32_BIT_UUID;
		break;
	case GATT_UUID_16_LEN:
		*p_data++ = GAP_AD_TYPE_COMPLETE_LIST_16_BIT_UUID;
		break;
	default:
		LOG_ERR("Failed to set advertising data with error %u", err);
		app_transition_to(APP_STATE_ERROR);
		co_buf_release(p_buf);
		return GAP_ERR_INVALID_PARAM;
	}
	memcpy(p_data, service_uuid, sizeof(service_uuid));

	err = gapm_le_set_adv_data(actv_idx, p_buf);
	co_buf_release(p_buf);

	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to set advertising data with error %u", err);
		app_transition_to(APP_STATE_ERROR);
	}

	return err;
}

static uint16_t set_scan_resp_data(uint8_t const actv_idx)
{
	co_buf_t *p_buf;
	uint16_t err = co_buf_alloc(&p_buf, 0, 0, 0);

	__ASSERT(err == 0, "Buffer allocation failed");

	err = gapm_le_set_scan_response_data(actv_idx, p_buf);
	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to set scan data with error %u", err);
		app_transition_to(APP_STATE_ERROR);
	}

	co_buf_release(p_buf);

	return err;
}

/* ---------------------------------------------------------------------------------------- */
/* Advertising callbacks */

static void start_le_adv(uint8_t const actv_idx)
{
	gapm_le_adv_param_t adv_params = {
		/* Advertise indefinitely */
		.duration = 0,
	};
	LOG_DBG("Starting advertising...");
	uint16_t const err = gapm_le_start_adv(actv_idx, &adv_params);

	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to start LE advertising with error %u", err);
		app_transition_to(APP_STATE_ERROR);
	}
}

static void on_adv_actv_stopped(uint32_t metainfo, uint8_t actv_idx, uint16_t reason)
{
	if (reason != GAP_ERR_NO_ERROR) {
		LOG_ERR("Advertising activity index %u stopped for reason %u", actv_idx, reason);
		app_transition_to(APP_STATE_ERROR);
		return;
	}
	printk("Client connected!\r\n");
}

static void on_adv_actv_proc_cmp(uint32_t metainfo, uint8_t proc_id, uint8_t actv_idx,
				 uint16_t status)
{
	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Advertising activity process completed with error %u", status);
		return;
	}

	switch (proc_id) {
	case GAPM_ACTV_CREATE_LE_ADV:
		LOG_DBG("Advertising activity is created");
		env.adv_actv_idx = actv_idx;
		set_advertising_data(actv_idx);
		break;

	case GAPM_ACTV_SET_ADV_DATA:
		LOG_DBG("Advertising data is set");
		set_scan_resp_data(actv_idx);
		break;

	case GAPM_ACTV_SET_SCAN_RSP_DATA:
		LOG_DBG("Scan data is set");
		start_le_adv(actv_idx);
		break;

	case GAPM_ACTV_START:
		LOG_DBG("Advertising was started");
		k_sem_give(&adv_started_sem);
		break;

	default:
		LOG_WRN("Unexpected GAPM activity complete, proc_id %u", proc_id);
		break;
	}
}

static void on_adv_created(uint32_t const metainfo, uint8_t const actv_idx, int8_t const tx_pwr)
{
	LOG_DBG("Advertising activity created, index %u, selected tx power %d", actv_idx, tx_pwr);
}

static void on_ext_adv_stopped(uint32_t const metainfo, uint8_t const actv_idx,
			       uint16_t const reason)
{
	LOG_DBG("Extended advertising activity stopped, index %u, reason=%d", actv_idx, reason);
}

static uint16_t create_advertising(void)
{
	static const gapm_le_adv_cb_actv_t le_adv_cbs = {
		.hdr.actv.stopped = on_adv_actv_stopped,
		.hdr.actv.proc_cmp = on_adv_actv_proc_cmp,
		.created = on_adv_created,
		.ext_adv_stopped = on_ext_adv_stopped,
	};

	gapm_le_adv_create_param_t adv_cfg = {
		.prop = GAPM_ADV_PROP_UNDIR_CONN_MASK,
		.disc_mode = GAPM_ADV_MODE_GEN_DISC,
#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
		.tx_pwr = 0,
#else
#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
		.tx_pwr = 0,
#else
		.max_tx_pwr = 0,
#endif /* !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 */
#endif /* !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 */
		.filter_pol = GAPM_ADV_ALLOW_SCAN_ANY_CON_ANY,
		.prim_cfg =
			{
				.adv_intv_min = 160,
				.adv_intv_max = 500,
				.ch_map = ADV_ALL_CHNLS_EN,
				.phy = GAPM_PHY_TYPE_LE_1M,
			},
	};
	uint16_t const err = gapm_le_create_adv_legacy(0, GAPM_STATIC_ADDR, &adv_cfg, &le_adv_cbs);

	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("Error %u creating advertising activity", err);
		return -1;
	}

	k_sem_take(&adv_started_sem, K_FOREVER);
	return 0;
}

/* ---------------------------------------------------------------------------------------- */
/* Public methods */

void peripheral_app_init(void)
{
	uint16_t status;

	static const gatt_srv_cb_t gatt_cbs = {
		.cb_att_event_get = NULL,
		.cb_att_info_get = NULL,
		.cb_att_read_get = on_att_read_get,
		.cb_att_val_set = on_att_val_set,
		.cb_event_sent = on_event_sent,
	};

	/* Register a GATT user */
	status = gatt_user_srv_register(CONFIG_BLE_MTU_SIZE, 0, &gatt_cbs, &env.user_lid);
	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("GATT user register failed. status=%u", status);
		app_transition_to(APP_STATE_ERROR);
		return;
	}

	/* Add the GATT service */
	status = gatt_db_svc_add(env.user_lid, SVC_UUID(128), service_uuid, LBS_IDX_NB, NULL,
				 lbs_att_db, LBS_IDX_NB, &env.start_hdl);
	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("GATT service add failed. status=%u", status);
		gatt_user_unregister(env.user_lid);
		app_transition_to(APP_STATE_ERROR);
	}
}

int peripheral_app_exec(uint32_t const app_state)
{
	switch (app_state) {
	case APP_STATE_PERIPHERAL_START_ADVERTISING: {
		create_advertising();
		app_transition_to(APP_STATE_STANDBY);
		break;
	}
	case APP_STATE_DISCONNECTED: {
		printk("Disconnected! Restart advertising\r\n");
		start_le_adv(env.adv_actv_idx);
		app_transition_to(APP_STATE_STANDBY);
		break;
	}
	case APP_STATE_PERIPHERAL_RECEIVING: {
		k_sleep(K_MSEC(100));
		break;
	}
	case APP_STATE_PERIPHERAL_PREPARE_SENDING: {
		env.start_time = k_uptime_get_32();
		env.mtu = gatt_bearer_mtu_min_get(0);
		env.total_len = 0;
		env.cnt = 0;

		printk("\r\n <<< transmit starts\r\n");
		k_sem_give(&app_sem);

		app_transition_to(APP_STATE_PERIPHERAL_SENDING);
		break;
	}
	case APP_STATE_PERIPHERAL_SENDING: {
		notification_send();
		break;
	}
	case APP_STATE_PERIPHERAL_SEND_RESULTS: {
		int const err = indication_send(&env.resp_data, sizeof(env.resp_data));

		if (err != 0) {
			LOG_ERR("Indication send error: failed to send data");
			app_transition_to(APP_STATE_ERROR);
			return err;
		}

		app_transition_to(APP_STATE_STANDBY);
		break;
	}
	default:
		k_sleep(K_MSEC(100));
		break;
	}
	return 0;
}

int peripheral_get_service_uuid_str(char *const p_uuid, uint8_t const max_len)
{
	return convert_uuid_with_len_to_string(p_uuid, max_len, service_uuid, sizeof(service_uuid));
}

K_SEM_DEFINE(gapm_cmp_wait_sem, 0, 1);

static void on_gapc_proc_cmp_cb(uint8_t const conidx, uint32_t const metainfo,
				uint16_t const status)
{
	ARG_UNUSED(conidx);
	ARG_UNUSED(metainfo);

	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("gapc_le_update_params failed. status=%u", status);
		if (status == GAP_ERR_DISCONNECTED) {
			app_transition_to(APP_STATE_DISCONNECTED);
		} else {
			app_transition_to(APP_STATE_ERROR);
		}
	} else {
		LOG_INF("LE Parameter update success");
	}
	k_sem_give(&gapm_cmp_wait_sem);
}

int peripheral_connection_params_set(struct peripheral_conn_params const *p_params)
{
	if (!p_params) {
		return -EINVAL;
	}

	const gapc_le_con_param_nego_with_ce_len_t preferred_connection_param = {
		.ce_len_min = 5,
		.ce_len_max = 10,
		.hdr.interval_min = p_params->conn_interval_min,
		.hdr.interval_max = p_params->conn_interval_max,
		.hdr.latency = 0,
		.hdr.sup_to = p_params->supervision_to};

	uint16_t const ret =
		gapc_le_update_params(0, 0, &preferred_connection_param, on_gapc_proc_cmp_cb);

	if (ret != GAP_ERR_NO_ERROR) {
		LOG_ERR("gapc_le_update_params failed. status=%u", ret);
		return -EINVAL;
	}

	LOG_INF("Updating connection params... waiting ready for 10seconds");
	if (k_sem_take(&gapm_cmp_wait_sem, K_SECONDS(10)) != 0) {
		LOG_ERR("Param update not ready");
		app_transition_to(APP_STATE_ERROR);
		return -ENOEXEC;
	}

	return 0;
}
