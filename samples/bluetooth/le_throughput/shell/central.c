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
#include "gapm_le_scan.h"
#include "gapm_le_init.h"
#include "gatt_db.h"
#include "gatt_srv.h"

#include "common.h"
#include "central.h"
#include "config.h"
#include "gatt_client.h"
#include "service_uuid.h"

#include "rom_build_cfg.h"

LOG_MODULE_REGISTER(central, LOG_LEVEL_ERR);

#if CONFIG_BLE_MTU_SIZE < L2CAP_COC_MTU_MIN
#error "MTU size is too small!"
#endif

#if (60 * 60 * 1000) <= CONFIG_BLE_THROUGHPUT_DURATION
#error "Test duration maximum is an hour!"
#endif

#define READ_SIZE      64
#define WRITE_SIZE     CONFIG_BLE_MTU_SIZE - GATT_BUFFER_HEADER_LEN - GATT_BUFFER_TAIL_LEN
#define WRITE_SIZE_MAX CFG_MAX_LE_MTU - GATT_BUFFER_HEADER_LEN - GATT_BUFFER_TAIL_LEN

#define CONNECT_INTERVAL_MIN_DEFAULT 6
#define CONNECT_INTERVAL_MAX_DEFAULT 200
#define SUPERVISION_TIMEOUT_DEFAULT  300

/* Environment for the service */
struct service_env {
	/* Delay between data sends (ms) */
	uint32_t send_interval_ms;
	/* Test duration (ms) */
	uint32_t test_duration_ms;
	/* Connection interval minimum (ms) */
	uint32_t conn_interval_min;
	/* Connection interval maximum (ms) */
	uint32_t conn_interval_max;
	/* Supervision timeout (ms) */
	uint32_t supervision_to;
	/* Peripheral BD address */
	gap_bdaddr_t periph_addr;
	/* SCAN activity index */
	uint8_t scan_actv_idx;
	/* INIT activity index */
	uint8_t init_actv_idx;
	/* Connection index */
	uint8_t conidx;
	/* Used to know if peripheral found */
	bool periph_found;
};

static struct service_env env = {
	.conn_interval_min = CONNECT_INTERVAL_MIN_DEFAULT,
	.conn_interval_max = CONNECT_INTERVAL_MAX_DEFAULT,
	.supervision_to = SUPERVISION_TIMEOUT_DEFAULT,
	.scan_actv_idx = GAP_INVALID_ACTV_IDX,
	.init_actv_idx = GAP_INVALID_ACTV_IDX,
};

struct tp_data transmit_throughput_results;
struct tp_data receive_throughput_results;
struct tp_data tp_stats;

static const char periph_device_name[] = CONFIG_BLE_TP_DEVICE_NAME;
static uint8_t tx_buffer[WRITE_SIZE];
static struct conn_handle service_handle;

K_SEM_DEFINE(scan_sem, 0, 1);

static bool is_uuid_match_in_report(co_buf_t *p_data)
{
	uint8_t len;
	uint8_t service_uuid[] = SERVICE_UUID;

	const uint8_t *data =
		gapm_get_ltv_value(GAP_AD_TYPE_COMPLETE_LIST_128_BIT_UUID, co_buf_data_len(p_data),
				   co_buf_data(p_data), &len);

	if (data && (len == sizeof(service_uuid)) && (memcmp(data, service_uuid, len) == 0)) {
		return true;
	}

	return false;
}

static bool is_match_in_report(co_buf_t *p_data, const char *p_exp_name)
{

	uint8_t peer_name_len;
	const uint8_t *p_peer_name =
		gapm_get_ltv_value(GAP_AD_TYPE_COMPLETE_NAME, co_buf_data_len(p_data),
				   co_buf_data(p_data), &peer_name_len);

	if (p_peer_name && (peer_name_len == strlen(p_exp_name)) &&
	    (memcmp(p_peer_name, p_exp_name, peer_name_len) == 0)) {
		return true;
	} else if (is_uuid_match_in_report(p_data)) {
		return true;
	}

	return false;
}

static void pretty_print_result(struct tp_data *p_data)
{
	if (!p_data) {
		LOG_ERR("Invalid data pointer");
		return;
	}

	char buff[32];
	double rate = p_data->write_rate;
	const char *unit = "bps";

	if (rate > (1024 * 1024)) {
		rate = rate / (1024 * 1024);
		unit = "Mbps";
	} else if (rate > 1024) {
		rate = rate / 1024;
		unit = "Kbps";
	}

	snprintf(buff, sizeof(buff), "%.2f %s", rate, unit);
	printk("%u packets, %u bytes @ %s\r\n", p_data->write_count, p_data->write_len, buff);
}

/* ---------------------------------------------------------------------------------------- */
/* Scanning */

static void on_scan_proc_cmp(uint32_t token, uint8_t proc_id, uint8_t actv_idx, uint16_t status)
{
	LOG_DBG("Scan process completed. (id: %u, pid: %u). Status: %u", actv_idx, proc_id, status);
	if (status == GAP_ERR_NO_ERROR) {
		if (proc_id == GAPM_ACTV_START) {
			app_transition_to(APP_STATE_SCAN_ONGOING);
		} else if (proc_id == GAPM_ACTV_DELETE) {
			k_sem_give(&scan_sem);
		}
	}
}

static void on_scan_stopped(uint32_t token, uint8_t actv_idx, uint16_t reason)
{
	app_transition_to(APP_STATE_SCAN_READY);
	k_sem_give(&scan_sem);
}

static void on_scan_report_received(uint32_t metainfo, uint8_t actv_idx,
				    const gapm_le_adv_report_info_t *p_info, co_buf_t *p_report)
{
	char temp_name[128] = "-";
	uint8_t peer_name_len = 0;
	const char *p_peer_name =
		(char *)gapm_get_ltv_value(GAP_AD_TYPE_COMPLETE_NAME, co_buf_data_len(p_report),
					   co_buf_data(p_report), &peer_name_len);
	if (peer_name_len && p_peer_name) {
		if (peer_name_len < 128) {
			memcpy(temp_name, p_peer_name, peer_name_len);
			temp_name[peer_name_len] = 0;
		} else {
			strcpy(temp_name, "ERROR!");
		}
	}

	if (!is_match_in_report(p_report, periph_device_name)) {
		return;
	}

	scan_stop(false);

	printk("Peripheral found! connecting...\r\n");

	env.periph_addr = p_info->trans_addr;
	env.periph_found = true;

	app_transition_to(APP_STATE_STANDBY);
}

/* Callback structure required to create a scan activity */
static const gapm_le_scan_cb_actv_t scan_activity_callbacks = {
	.le_actv.actv.stopped = on_scan_stopped,
	.le_actv.actv.proc_cmp = on_scan_proc_cmp,
	.le_actv.addr_updated = NULL,
	.report_received = on_scan_report_received,
};

int scan_stop(bool const sync)
{
	LOG_INF("Stop scanning (id: %d)", env.scan_actv_idx);
	uint16_t const status = gapm_stop_activity(env.scan_actv_idx);

	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Scan stop failed! Error: %u", status);
		return -1;
	}

	if (sync && k_sem_take(&scan_sem, K_MSEC(200)) != 0) {
		LOG_ERR("Scan stop not ready in 200ms");
		return -2;
	}
	return 0;
}

int scan_create_and_start(struct scan_config const *const p_config)
{
	if (APP_STATE_SCAN_ONGOING == get_app_state()) {
		scan_stop(true);
	}

	env.periph_found = false;

	gapm_le_scan_param_t param = {
		.type = 0,
		.prop = 0,
		.dup_filt_pol = p_config->filter,
		.scan_param_1m.scan_intv = p_config->interval,
		.scan_param_1m.scan_wd = p_config->window,
		.scan_param_coded.scan_intv = p_config->interval,
		.scan_param_coded.scan_wd = p_config->window,
		/* Infinite scan, explicit stop required */
		.duration = 0,
		.period = 0,
	};

	switch (p_config->type) {
	case SCAN_TYPE_CONNECTABLE:
		param.type = GAPM_SCAN_TYPE_CONN_DISC;
		break;
	case SCAN_TYPE_LIMITED:
		param.type = GAPM_SCAN_TYPE_LIM_DISC;
		break;
	case SCAN_TYPE_GENERIC:
	default:
		param.type = GAPM_SCAN_TYPE_GEN_DISC;
		break;
	}

	switch (p_config->properties) {
	case SCAN_PROP_ACTIVE_1M:
		param.prop |= GAPM_SCAN_PROP_ACTIVE_1M_BIT;
	case SCAN_PROP_PHY_1M:
		param.prop |= GAPM_SCAN_PROP_PHY_1M_BIT;
		break;
	case SCAN_PROP_ACTIVE_CODED:
		param.prop |= GAPM_SCAN_PROP_ACTIVE_CODED_BIT;
	case SCAN_PROP_PHY_CODED:
		param.prop |= GAPM_SCAN_PROP_PHY_CODED_BIT;
		break;
	case SCAN_PROP_FILT_TRUNK:
		param.prop = GAPM_SCAN_PROP_FILT_TRUNC_BIT;
		break;
	default:
		break;
	}

	LOG_INF("Start scanning: type=%u, prop=%u, filter=%u, interval=%u, window=%u", param.type,
		param.prop, param.dup_filt_pol, param.scan_param_1m.scan_intv,
		param.scan_param_1m.scan_wd);
	printk("Start scanning\r\n");

	uint16_t status;

	if (env.scan_actv_idx == GAP_INVALID_ACTV_IDX) {
		status = gapm_le_create_scan(0, GAPM_STATIC_ADDR, &scan_activity_callbacks,
					     &env.scan_actv_idx);
		if (status != GAP_ERR_NO_ERROR) {
			LOG_ERR("Scan create failed. Status %u", status);
			return -1;
		}
	}

	status = gapm_le_start_scan(env.scan_actv_idx, &param);
	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Scan start failed. Status %u", status);
		return -1;
	}
	return 0;
}

static int scan_remove(void)
{
	if (GAP_INVALID_ACTV_IDX == env.scan_actv_idx) {
		LOG_INF("Delete scanning (id: %d)", env.scan_actv_idx);
		uint16_t const status = gapm_delete_activity(env.scan_actv_idx);

		if (status != GAP_ERR_NO_ERROR) {
			LOG_ERR("Scan stop failed! Error: %u", status);
			return -1;
		}

		/* Wait until ready */
		if (k_sem_take(&scan_sem, K_MSEC(200)) != 0) {
			LOG_ERR("Scan delete not ready in 200ms");
			return -2;
		}

		env.scan_actv_idx = GAP_INVALID_ACTV_IDX;
	}
	return 0;
}

/* ---------------------------------------------------------------------------------------- */
/* Connection establishement */

static void app_init_proc_cmp(uint32_t token, uint8_t proc_id, uint8_t actv_idx, uint16_t status)
{
	LOG_DBG("INIT_PROC_CMP(%d, %d, %d, %d[0x%X])", token, proc_id, actv_idx, status, status);

	if (status != GAP_ERR_NO_ERROR) {
		app_transition_to(APP_STATE_STANDBY);
		return;
	}

	switch (proc_id) {
	case GAPM_ACTV_START: {
		break;
	}
	case GAPM_ACTV_DELETE: {
		env.init_actv_idx = GAP_INVALID_ACTV_IDX;
		break;
	}
	default:
		break;
	}
}

static void app_init_stopped(uint32_t token, uint8_t actv_idx, uint16_t reason)
{
	LOG_DBG("INIT_STOPPED(%d, %d, %d)", token, actv_idx, reason);
	if (reason != GAP_ERR_NO_ERROR) {
		app_transition_to(APP_STATE_DISCONNECTED);
	}
}

static const gapm_le_init_cb_actv_t app_init_actv_cb_itf = {
	.hdr.actv.stopped = app_init_stopped,
	.hdr.actv.proc_cmp = app_init_proc_cmp,
	.hdr.addr_updated = NULL,
	.peer_name = NULL,
};

static int connection_create_and_start(void)
{
	if (!env.periph_found) {
		LOG_ERR("No peripheral available to connect...");
		return -2;
	}

	const uint32_t conn_intv_min = env.conn_interval_min;
	const uint32_t conn_intv_max = env.conn_interval_max;
	const uint32_t supervision_to = env.supervision_to;
	const uint32_t ce_len_min = 5;
	const uint32_t ce_len_max = 10;

	gapm_le_init_param_t param = {
		.prop = (GAPM_INIT_PROP_1M_BIT | GAPM_INIT_PROP_2M_BIT),
		/* Timeout for automatic connection establishment (in unit of 10ms) */
		.conn_to = 100,

		.scan_param_1m.scan_intv = 160,  /* Scan interval (N * 0.625 ms) = 100ms */
		.scan_param_1m.scan_wd = 80,     /* Scan window (N * 0.625 ms) = 50ms */
		.scan_param_coded.scan_intv = 0, /* disabled */
		.scan_param_coded.scan_wd = 0,   /* disabled */

		.conn_param_1m.conn_intv_min = conn_intv_min,
		.conn_param_1m.conn_intv_max = conn_intv_max,
		.conn_param_1m.conn_latency = 0,
		.conn_param_1m.supervision_to = supervision_to,
		.conn_param_1m.ce_len_min = ce_len_min,
		.conn_param_1m.ce_len_max = ce_len_max,

		.conn_param_2m.conn_intv_min = conn_intv_min,
		.conn_param_2m.conn_intv_max = conn_intv_max,
		.conn_param_2m.conn_latency = 0,
		.conn_param_2m.supervision_to = supervision_to,
		.conn_param_2m.ce_len_min = ce_len_min,
		.conn_param_2m.ce_len_max = ce_len_max,

		.conn_param_coded.conn_intv_min = conn_intv_min,
		.conn_param_coded.conn_intv_max = conn_intv_max,
		.conn_param_coded.conn_latency = 0,
		.conn_param_coded.supervision_to = supervision_to,
		.conn_param_coded.ce_len_min = ce_len_min,
		.conn_param_coded.ce_len_max = ce_len_max,

		.peer_addr = env.periph_addr,
	};

	/* create activity */
	uint16_t status;

	if (env.init_actv_idx == GAP_INVALID_ACTV_IDX) {
		status = gapm_le_create_init(0, GAPM_STATIC_ADDR, &app_init_actv_cb_itf,
					     &env.init_actv_idx);
		if (status != GAP_ERR_NO_ERROR) {
			LOG_ERR("LE activity create failed: %u", status);
			return -1;
		}
	}

	/* start it */
	status = gapm_le_start_direct_connection(env.init_actv_idx, &param);
	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Direct connect start failed: %u", status);
		return -1;
	}

	LOG_DBG("env.init_actv_idx=%u", env.init_actv_idx);

	return 0;
}

/* ---------------------------------------------------------------------------------------- */
/* Feature req */

/* Metainfo for GAPC procedures */
enum applet_le_connection_gapc_metainfo {
	/* Disconnect */
	GAPC_METAINFO_DISCONNECT = 0U,
	/* Set Authentication Payload Timeout (LE Ping) */
	GAPC_METAINFO_LE_PING,
	/* Get list of peer supported features */
	GAPC_METAINFO_PEER_FEATURES,
	/* Update subrating */
	GAPC_METAINFO_UPDATE_SUBRATING,
	/* Use Periodic Advertising Sync Transfer */
	GAPC_METAINFO_PAST,
};

static void applet_le_connection_cb_gapc_peer_features(uint8_t conidx, uint32_t metainfo,
						       uint16_t status, const uint8_t *p_features)
{
	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Peer info feature failure! status=%u", status);
		app_transition_to(APP_STATE_STANDBY);
		return;
	}

	LOG_DBG("Peer features (conidx = %d) - %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X", conidx,
		p_features[0], p_features[1], p_features[2], p_features[3], p_features[4],
		p_features[5], p_features[6], p_features[7]);
	app_transition_to(APP_STATE_DISCOVER_SERVICES);
}

/* ---------------------------------------------------------------------------------------- */
/* Public methods */

static uint16_t tx_size;

void central_app_init(void)
{
	gatt_client_register();
	tx_size = sizeof(tx_buffer);

	env.test_duration_ms = CONFIG_BLE_THROUGHPUT_DURATION;
}

int central_app_exec(uint32_t const app_state)
{

	static uint32_t last_tp_read;

	uint32_t const current_ms = k_uptime_get_32();

	switch (app_state) {
	case APP_STATE_SCAN_START: {
		if (scan_create_and_start_default() < 0) {
			k_sleep(K_MSEC(1000));
		}
		app_transition_to(APP_STATE_STANDBY);
		break;
	}

	case APP_STATE_SCAN_READY: {
		scan_remove();
		app_transition_to(env.periph_found ? APP_STATE_PERIPHERAL_FOUND
						   : APP_STATE_STANDBY);
		break;
	}

	case APP_STATE_PERIPHERAL_FOUND: {
		LOG_INF("Connecting...");
		if (connection_create_and_start()) {
			app_transition_to(APP_STATE_STANDBY);
		} else {
			app_transition_to(APP_STATE_CONNECTING);
		}
		break;
	}
	case APP_STATE_CONNECTING: {
		k_sleep(K_MSEC(100));
		break;
	}
	case APP_STATE_CONNECTED: {
		LOG_INF("Connected! Collecting infos...");
		app_transition_to(APP_STATE_DISCOVER_SERVICES);
		break;
	}
	case APP_STATE_CONNECTED_PAIRED: {
		LOG_INF("Paired! Starting data transfer...");
		break;
	}
	case APP_STATE_DISCONNECTED: {
		LOG_INF("Disconnected! Restart scanning...");
		app_transition_to(APP_STATE_SCAN_START);
		break;
	}

	case APP_STATE_GET_FEATURES: {
		/*  Get client features, not used ATM */
		LOG_INF("Get features...");
		gapc_le_get_peer_features(get_connection_index(), GAPC_METAINFO_PEER_FEATURES,
					  applet_le_connection_cb_gapc_peer_features);
		app_transition_to(APP_STATE_STANDBY);
		break;
	}
	case APP_STATE_DISCOVER_SERVICES: {
		LOG_INF("Discover services...");
		service_handle.conidx = get_connection_index();
		uint8_t uuid[] = SERVICE_UUID;
		struct conn_uuid const uuid_config = {
			.p_uuid = uuid,
			.uuid_type = GATT_UUID_128,
			.conidx = service_handle.conidx,
		};
		int res = gatt_client_discover_primary_by_uuid(uuid_config, &service_handle.handle);

		tx_size = get_mtu_size();
		if (!tx_size) {
			LOG_ERR("Invalid MTU size received: %u", tx_size);
			res = -1;
		} else if (WRITE_SIZE_MAX < tx_size) {
			LOG_ERR("MTU size is too big!: max=%u < %u", WRITE_SIZE_MAX, tx_size);
			res = -1;
		}
		LOG_INF("Using MTU size: %u", tx_size);

		gatt_client_register_event(service_handle);

		if (res < 0) {
			app_transition_to(APP_STATE_ERROR);
		} else {
			printk("Type 'tp run' to start test\r\n");
			app_transition_to(APP_STATE_CENTRAL_READY);
		}
		break;
	}

	case APP_STATE_CENTRAL_READY: {
		k_sleep(K_MSEC(100));
		break;
	}
	case APP_STATE_DATA_TRANSMIT: {
		if (gatt_client_write_noack(service_handle, (void *)tx_buffer, tx_size)) {
			app_transition_to(APP_STATE_ERROR);
		}

		size_t const current_cnt = tp_stats.write_count + 1;
		size_t const current_len = tp_stats.write_len + tx_size;

		tp_stats.write_count = current_cnt;
		tp_stats.write_len = current_len;

		if ((current_cnt % 256) == 0) {
			printk(".");
		}

		if (env.send_interval_ms) {
			k_sleep(K_MSEC(env.send_interval_ms));
		}

		if (env.test_duration_ms <= (current_ms - last_tp_read)) {
			printk("\r\n");
			app_transition_to(APP_STATE_DATA_READ);
		}

		break;
	}

	case APP_STATE_DATA_READ: {
		app_transition_to(APP_STATE_CENTRAL_READY);
		LOG_DBG("Reading results");
		gatt_client_read(service_handle, READ_SIZE);
		break;
	}

	case APP_STATE_DATA_SEND_READY: {
		LOG_DBG("Transmit test ready");
		LOG_DBG("Sent %u packets %u bytes %u bps", tp_stats.write_count, tp_stats.write_len,
			((tp_stats.write_len << 3) / ((current_ms - last_tp_read) / 1000)));
		printk(" >>> TRASMIT RESULT: ");
		pretty_print_result(&transmit_throughput_results);

		if (IS_ENABLED(CONFIG_BLE_TP_BIDIRECTIONAL_TEST)) {
			printk("\r\n <<< Reception test starts\r\n");
		}

		app_transition_to(APP_STATE_CENTRAL_READY);
		memset(&tp_stats, 0, sizeof(tp_stats));
		break;
	}

	case APP_STATE_DATA_RECEIVE_READY: {
		LOG_DBG("Peer sent %u bytes %u packets in %u bps", tp_stats.write_len,
			tp_stats.write_count, tp_stats.write_rate);
		printk(" <<< RECEIVE RESULT: ");
		pretty_print_result(&receive_throughput_results);
		app_transition_to(APP_STATE_CENTRAL_READY);
		break;
	}

	case APP_STATE_STATS_RESET: {
		/* Clear a buffer by sending a byte */
		struct tp_client_ctrl command = {
			.type = TP_CLIENT_CTRL_TYPE_RESET,
			.test_duration_ms = env.test_duration_ms,
			.send_interval_ms = env.send_interval_ms,
		};

		memset(&tp_stats, 0, sizeof(tp_stats));

		gatt_client_write_noack(service_handle, (void *)&command, sizeof(command));

		last_tp_read = current_ms;

		app_transition_to(APP_STATE_DATA_TRANSMIT);
		printk(" >>> Transmit test starts\r\n");
		break;
	}

	case APP_STATE_SCAN_ONGOING:
		k_sleep(K_MSEC(100));
		break;

	default:
		/* Invalid state */
		return -1;
	}

	return 0;
}

int scan_create_and_start_default(void)
{
	struct scan_config defaults = {
		.interval = 160,
		.window = 80,
		.type = SCAN_TYPE_GENERIC,
		.properties = SCAN_PROP_ACTIVE_1M,
		.filter = true,
	};
	return scan_create_and_start(&defaults);
}

int central_get_service_uuid_str(char *p_uuid, uint8_t max_len)
{
	uint8_t service_uuid[] = SERVICE_UUID;

	return convert_uuid_with_len_to_string(p_uuid, max_len, service_uuid, sizeof(service_uuid));
}

int central_set_send_interval(uint32_t const interval)
{
	env.send_interval_ms = interval;

	return 0;
}

static int central_set_connection_interval(uint32_t const interval_min, uint32_t const interval_max)
{
	if (interval_min < 6 || interval_min > 3200) {
		LOG_ERR("connection interval min out of bounds: %u", interval_min);
		return -EINVAL;
	}

	if (interval_max < 6 || interval_max > 3200) {
		LOG_ERR("connection interval max out of bounds: %u", interval_max);
		return -EINVAL;
	}

	if (interval_max < interval_min) {
		LOG_ERR("connection interval min cannot be greater than max!");
		return -EINVAL;
	}

	env.conn_interval_min = interval_min;
	env.conn_interval_max = interval_max;

	LOG_INF("connection interval set to min: %fms max: %fms", 1.25 * interval_min,
		1.25 * interval_max);

	return 0;
}

static int central_set_supervision_timeout(uint32_t const timeout)
{
	if (timeout < 10 || 3200 < timeout) {
		LOG_ERR("supervision timeout out of bounds: %u", timeout);
		return -EINVAL;
	}

	env.supervision_to = timeout;

	LOG_INF("supervision timeout set to %dms", timeout * 10);

	return 0;
}

int central_connection_params_get(struct central_conn_params *const p_params)
{
	if (!p_params) {
		return -EINVAL;
	}

	p_params->conn_interval_min = env.conn_interval_min;
	p_params->conn_interval_max = env.conn_interval_max;
	p_params->supervision_to = env.supervision_to;

	return 0;
}

int central_connection_params_set(struct central_conn_params const *const p_params)
{
	if (!p_params) {
		return -EINVAL;
	}

	const int err = central_set_supervision_timeout(p_params->supervision_to);

	if (err) {
		return err;
	}

	return central_set_connection_interval(p_params->conn_interval_min,
					       p_params->conn_interval_max);
}

int central_set_test_duration(uint32_t const duration_s)
{
	if (duration_s < 2 || duration_s > (60 * 60)) {
		LOG_ERR("Test duration maximum is an hour!");
		return -EINVAL;
	}

	env.test_duration_ms = 1000LU * duration_s;

	return 0;
}
