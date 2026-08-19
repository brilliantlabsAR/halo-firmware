/** @file
 * @brief Bluetooth shell module
 *
 * Provide some Bluetooth shell commands that can be useful to applications.
 *
 * Copyright (c) 2017 Intel Corporation
 * Copyright (c) 2018 Nordic Semiconductor ASA
 * Copyright (c) Alif Semiconductor
 *
 * Alif Semiconductor version uses the Zephyr's bt shell module as a base
 * but adapts it to the Ceva-Waves' BLE stack.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/sys/util.h>
#include <zephyr/sys/printk.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
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
#include "gatt_db.h"
#include "gatt_srv.h"
#include "ke_mem.h"

#include <alif/bluetooth/bt_adv_data.h>
#include <alif/bluetooth/bt_scan_rsp.h>
#include <alif/bluetooth/bt_srv_hello.h>

LOG_MODULE_REGISTER(bt_shell, CONFIG_BT_SHELL_LOG_LEVEL);

#define BLE_MUTEX_TIMEOUT_MS       10000
#define BT_CONN_STATE_CONNECTED	   0x00
#define BT_CONN_STATE_DISCONNECTED 0x01

/*
 * Semaphores
 */
static K_SEM_DEFINE(bt_init_sem, 0, 1);
static K_SEM_DEFINE(bt_process_sem, 0, 1);

/*
 * Connection parameters
 */
static struct connections_params {
	bool no_settings_load; /* Start without loading settings */
	bool bt_initialized;   /* Bluetooth initialized */
	uint8_t status;        /* Connection status */
} cxn;

/*
 * Advertising parameters
 */
static struct adv_params {
	gapm_le_adv_create_param_t param;
	bool valid;       /* Are the stored advertising parameters valid */
	uint8_t actv_idx; /* Activity index of the effective advertising parameters */
} stored_adv __attribute__((noinit));

char bt_device_name[CONFIG_BLE_DEVICE_NAME_MAX] = CONFIG_BLE_DEVICE_NAME;

/* Convert MAC address string from Kconfig to byte array */
static void init_private_addr(uint8_t *addr)
{
	/* Parse CONFIG_BT_SHELL_PRIVATE_ADDR string (format: XX:XX:XX:XX:XX:XX) */
	char addr_str[] = CONFIG_BT_SHELL_PRIVATE_ADDR;
	char *ptr = addr_str;
	char byte_str[3];

	byte_str[sizeof(byte_str) - 1] = '\0';

	/* Parse each byte of the address */
	for (int i = 5; i >= 0; i--) {
		/* Copy two hex chars */
		byte_str[0] = *ptr++;
		byte_str[1] = *ptr++;
		/* Convert to byte */
		addr[i] = strtoul(byte_str, NULL, 16);
		/* Skip colon */
		if (i > 0) {
			ptr++;
		}
	}
}

/* GAP role configuration based on Kconfig choice */
#if defined(CONFIG_BT_SHELL_GAP_ROLE_NONE)
#define BT_SHELL_GAP_ROLE GAP_ROLE_NONE
#elif defined(CONFIG_BT_SHELL_GAP_ROLE_LE_OBSERVER)
#define BT_SHELL_GAP_ROLE GAP_ROLE_LE_OBSERVER
#elif defined(CONFIG_BT_SHELL_GAP_ROLE_LE_BROADCASTER)
#define BT_SHELL_GAP_ROLE GAP_ROLE_LE_BROADCASTER
#elif defined(CONFIG_BT_SHELL_GAP_ROLE_LE_CENTRAL)
#define BT_SHELL_GAP_ROLE GAP_ROLE_LE_CENTRAL
#elif defined(CONFIG_BT_SHELL_GAP_ROLE_LE_PERIPHERAL)
#define BT_SHELL_GAP_ROLE GAP_ROLE_LE_PERIPHERAL
#elif defined(CONFIG_BT_SHELL_GAP_ROLE_LE_ALL)
#define BT_SHELL_GAP_ROLE GAP_ROLE_LE_ALL
#else
#error "Invalid GAP role configuration"
#endif

/* Pairing mode configuration */
#if defined(CONFIG_BT_SHELL_PAIRING_DISABLE)
#define BT_SHELL_PAIRING_MODE GAPM_PAIRING_DISABLE
#elif defined(CONFIG_BT_SHELL_PAIRING_LEGACY)
#define BT_SHELL_PAIRING_MODE GAPM_PAIRING_LEGACY
#elif defined(CONFIG_BT_SHELL_PAIRING_SEC_CON)
#define BT_SHELL_PAIRING_MODE GAPM_PAIRING_SEC_CON
#elif defined(CONFIG_BT_SHELL_PAIRING_CT2)
#define BT_SHELL_PAIRING_MODE GAPM_PAIRING_CT2
#elif defined(CONFIG_BT_SHELL_PAIRING_BT_SSP)
#define BT_SHELL_PAIRING_MODE GAPM_PAIRING_BT_SSP
#elif defined(CONFIG_BT_SHELL_PAIRING_ALL)
#define BT_SHELL_PAIRING_MODE GAPM_PAIRING_MODE_ALL
#else
#error "Invalid pairing mode configuration"
#endif

/* PHY preferences configuration */
#if defined(CONFIG_BT_SHELL_PHY_1MBPS_TX)
#define BT_SHELL_TX_PREF_PHY GAP_PHY_1MBPS
#elif defined(CONFIG_BT_SHELL_PHY_2MBPS_TX)
#define BT_SHELL_TX_PREF_PHY GAP_PHY_2MBPS
#elif defined(CONFIG_BT_SHELL_PHY_CODED_TX)
#define BT_SHELL_TX_PREF_PHY GAP_PHY_CODED
#elif defined(CONFIG_BT_SHELL_PHY_ANY_TX)
#define BT_SHELL_TX_PREF_PHY GAP_PHY_ANY
#else
#error "Invalid TX PHY preference configuration"
#endif

#if defined(CONFIG_BT_SHELL_PHY_1MBPS_RX)
#define BT_SHELL_RX_PREF_PHY GAP_PHY_1MBPS
#elif defined(CONFIG_BT_SHELL_PHY_2MBPS_RX)
#define BT_SHELL_RX_PREF_PHY GAP_PHY_2MBPS
#elif defined(CONFIG_BT_SHELL_PHY_CODED_RX)
#define BT_SHELL_RX_PREF_PHY GAP_PHY_CODED
#elif defined(CONFIG_BT_SHELL_PHY_ANY_RX)
#define BT_SHELL_RX_PREF_PHY GAP_PHY_ANY
#else
#error "Invalid RX PHY preference configuration"
#endif

/* Privacy configuration */
#if defined(CONFIG_BT_SHELL_PRIVACY_DISABLED)
#define BT_SHELL_PRIVACY_CFG 0 /* public address, controller privacy disabled*/
#elif defined(CONFIG_BT_SHELL_PRIVACY_ENABLED)
#define BT_SHELL_PRIVACY_CFG 1 /* static private random address, controller privacy disabled */
#else
#error "Invalid privacy configuration"
#endif

/* Bluetooth stack configuration*/
static gapm_config_t gapm_cfg = {
	.role = BT_SHELL_GAP_ROLE,
	.pairing_mode = BT_SHELL_PAIRING_MODE,
	.privacy_cfg = BT_SHELL_PRIVACY_CFG,
	.renew_dur = CONFIG_BT_SHELL_RENEW_DUR,
	.private_identity.addr = {0}, /* Will be set from Kconfig in init_private_addr() */
	.irk.key = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
	.gap_start_hdl = 0,
	.gatt_start_hdl = 0,
	.att_cfg = CONFIG_BT_SHELL_ATT_CFG,
	.sugg_max_tx_octets = CONFIG_BT_SHELL_MAX_TX_OCTETS,
	.sugg_max_tx_time = CONFIG_BT_SHELL_MAX_TX_TIME,
	.tx_pref_phy = BT_SHELL_TX_PREF_PHY,
	.rx_pref_phy = BT_SHELL_RX_PREF_PHY,
	.tx_path_comp = CONFIG_BT_SHELL_TX_PATH_COMP,
	.rx_path_comp = CONFIG_BT_SHELL_RX_PATH_COMP,
	.class_of_device = 0,  /* BT Classic only */
	.dflt_link_policy = 0, /* BT Classic only */
};

/**
 * Bluetooth GAPM callbacks
 */
static void on_le_connection_req(uint8_t conidx, uint32_t metainfo, uint8_t actv_idx, uint8_t role,
				 const gap_bdaddr_t *p_peer_addr,
				 const gapc_le_con_param_t *p_con_params, uint8_t clk_accuracy)
{
	LOG_INF("Connection request on index %u", conidx);
	/* No mutex needed - callback runs in BLE thread which already holds the mutex */
	gapc_le_connection_cfm(conidx, 0, NULL);

	LOG_INF("Connection parameters: interval %u, latency %u, supervision timeout %u",
		p_con_params->interval, p_con_params->latency, p_con_params->sup_to);

	LOG_INF("Peer BD address %02X:%02X:%02X:%02X:%02X:%02X (conidx: %u)", p_peer_addr->addr[5],
		p_peer_addr->addr[4], p_peer_addr->addr[3], p_peer_addr->addr[2],
		p_peer_addr->addr[1], p_peer_addr->addr[0], conidx);

	cxn.status = BT_CONN_STATE_CONNECTED;
}

static void on_key_received(uint8_t conidx, uint32_t metainfo, const gapc_pairing_keys_t *p_keys)
{
	LOG_INF("Unexpected key received key on conidx %u", conidx);
}

static void on_disconnection(uint8_t conidx, uint32_t metainfo, uint16_t reason)
{
	LOG_INF("Connection index %u disconnected for reason %u", conidx, reason);

	cxn.status = BT_CONN_STATE_DISCONNECTED;
}

static void on_name_get(uint8_t conidx, uint32_t metainfo, uint16_t token, uint16_t offset,
			uint16_t max_len)
{
	const size_t device_name_len = strlen(bt_device_name);
	const size_t short_len = (device_name_len > max_len ? max_len : device_name_len);

	/* No mutex needed - callback runs in BLE thread which already holds the mutex */
	gapc_le_get_name_cfm(conidx, token, GAP_ERR_NO_ERROR, device_name_len, short_len,
			     (const uint8_t *)bt_device_name);
}

static void on_appearance_get(uint8_t conidx, uint32_t metainfo, uint16_t token)
{
	/* Send 'unknown' appearance */
	/* No mutex needed - callback runs in BLE thread which already holds the mutex */
	gapc_le_get_appearance_cfm(conidx, token, GAP_ERR_NO_ERROR, 0);
}

static const gapc_connection_req_cb_t gapc_con_cbs = {
	.le_connection_req = on_le_connection_req,
};

static const gapc_security_cb_t gapc_sec_cbs = {
	.key_received = on_key_received,
	/* All other callbacks in this struct are optional */
};

static const gapc_connection_info_cb_t gapc_con_inf_cbs = {
	.disconnected = on_disconnection,
	.name_get = on_name_get,
	.appearance_get = on_appearance_get,
	/* Other callbacks in this struct are optional */
};

/* All callbacks in this struct are optional */
static const gapc_le_config_cb_t gapc_le_cfg_cbs;

#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
static void on_gapm_err(uint32_t metainfo, uint8_t code)
{
	LOG_ERR("GAPM operation failed with error code: 0x%02x", code);
}
static const gapm_cb_t gapm_err_cbs = {
	.cb_hw_error = on_gapm_err,
};

static const gapm_callbacks_t gapm_cbs = {
	.p_con_req_cbs = &gapc_con_cbs,
	.p_sec_cbs = &gapc_sec_cbs,
	.p_info_cbs = &gapc_con_inf_cbs,
	.p_le_config_cbs = &gapc_le_cfg_cbs,
	.p_bt_config_cbs = NULL, /* BT classic so not required */
	.p_gapm_cbs = &gapm_err_cbs,
};
#else
static void on_gapm_err(enum co_error err)
{
	LOG_ERR("GAPM operation failed with error code: 0x%02x", err);
}

static const gapm_err_info_config_cb_t gapm_err_cbs = {
	.ctrl_hw_error = on_gapm_err,
};

static const gapm_callbacks_t gapm_cbs = {
	.p_con_req_cbs = &gapc_con_cbs,
	.p_sec_cbs = &gapc_sec_cbs,
	.p_info_cbs = &gapc_con_inf_cbs,
	.p_le_config_cbs = &gapc_le_cfg_cbs,
	.p_bt_config_cbs = NULL, /* BT classic so not required */
	.p_err_info_config_cbs = &gapm_err_cbs,
};
#endif

/**
 * Advertising callbacks
 */
static void on_adv_actv_stopped(uint32_t metainfo, uint8_t actv_idx, uint16_t reason)
{
	LOG_INF("Advertising activity index %u stopped for reason %u", actv_idx, reason);
}

static void on_adv_actv_proc_cmp(uint32_t metainfo, uint8_t proc_id, uint8_t actv_idx,
				 uint16_t status)
{
	if (status) {
		LOG_ERR("Advertising activity failed with error code: 0x%02x", status);
		return;
	}

	switch (proc_id) {
	case GAPM_ACTV_CREATE_LE_ADV:
		LOG_INF("Created advertising activity");
		stored_adv.actv_idx = actv_idx;
		bt_adv_data_set_default(actv_idx, bt_device_name, strlen(bt_device_name));
		break;
	case GAPM_ACTV_SET_ADV_DATA:
		LOG_INF("Set advertising data");
		bt_scan_rsp_set(actv_idx);
		break;
	case GAPM_ACTV_SET_SCAN_RSP_DATA:
		LOG_INF("Set scan response data");
		break;
	case GAPM_ACTV_START:
		LOG_INF("Started advertising");
		break;
	case GAPM_ACTV_STOP:
		LOG_INF("Stopped advertising");
		break;
	case GAPM_ACTV_DELETE:
		LOG_INF("Deleted advertising activity");
		stored_adv.actv_idx = 0xFF;
		break;
	default:
		__ASSERT(false, "Received unexpected GAPM activity completion, proc_id %u",
			 proc_id);
		LOG_WRN("Received unexpected GAPM activity completion, proc_id %u", proc_id);
		break;
	}

	k_sem_give(&bt_process_sem);
}

static void on_adv_created(uint32_t metainfo, uint8_t actv_idx, int8_t tx_pwr)
{
	stored_adv.actv_idx = actv_idx;
	LOG_INF("Created advertising activity with index %u, tx power %d dBm", actv_idx, tx_pwr);
}

static const gapm_le_adv_cb_actv_t le_adv_cbs = {
	.hdr.actv.stopped = on_adv_actv_stopped,
	.hdr.actv.proc_cmp = on_adv_actv_proc_cmp,
	.created = on_adv_created,
};

static uint16_t service_init(void)
{
	int err;

	/* Initialize the hello service */
	err = bt_srv_hello_init();
	if (err) {
		LOG_ERR("Cannot initialize hello service, error code: %d", err);
		return GAP_ERR_INVALID_PARAM;
	}

	LOG_DBG("Hello service initialized");

	return GAP_ERR_NO_ERROR;
}

static void on_gapm_process_complete(uint32_t metainfo, uint16_t status)
{
	uint16_t err;

	if (status) {
		LOG_ERR("gapm process completed with error 0x%02x", status);
		goto end;
	}

	LOG_INF("GAPM configuration succeeded");

	err = service_init();

	if (err) {
		LOG_ERR("Cannot add BLE profile, error code: 0x%02x", err);
		goto end;
	}

	cxn.bt_initialized = true;

end:
	k_sem_give(&bt_init_sem);
}

static void on_ble_enabled(void)
{
	int err = gapm_configure(0, &gapm_cfg, &gapm_cbs, on_gapm_process_complete);

	if (err) {
		LOG_ERR("Cannot configure GAPM, error code: %u", err);
		return;
	}
}

static bool is_initialized(const struct shell *sh)
{
	if (!cxn.bt_initialized) {
		shell_error(sh, "BLE stack not initialized. Run 'bt init' first or wait for "
				"initialization to complete.");
		return false;
	}
	return true;
}

static int cmd_init(const struct shell *sh, size_t argc, char *argv[])
{
	int err;
	bool sync = false;

	stored_adv.actv_idx = 0xFF;
	stored_adv.valid = false;

	if (cxn.bt_initialized) {
		shell_error(sh, "BLE stack already initialized");
		return -EALREADY;
	}

	/* Initialize the private address from Kconfig directly into the gapm_cfg structure */
	init_private_addr((void *)gapm_cfg.private_identity.addr);

	for (size_t argn = 1; argn < argc; argn++) {
		const char *arg = argv[argn];

		if (!strcmp(arg, "no-settings-load")) {
			cxn.no_settings_load = true;
		} else if (!strcmp(arg, "sync")) {
			sync = true;
		} else {
			shell_help(sh);
			return SHELL_CMD_HELP_PRINTED;
		}
	}

	/* Enable only fails if it was already enabled */
	if (sync) {
		err = alif_ble_enable(NULL);

		int lock_ret = alif_ble_mutex_lock(K_MSEC(BLE_MUTEX_TIMEOUT_MS));

		if (lock_ret) {
			__ASSERT(false, "BLE mutex lock timeout");
			shell_error(sh, "BLE mutex lock timeout");
			return lock_ret;
		}
		on_ble_enabled();
		alif_ble_mutex_unlock();

		k_sem_take(&bt_init_sem, K_FOREVER);
	} else {
		err = alif_ble_enable(on_ble_enabled);
	}

	if (err) {
		shell_error(sh, "Failed to initialize BLE stack: %d", err);
	} else {
		shell_print(sh, "Initialized BLE stack");
	}

	return err;
}

static bool adv_param_parse(const struct shell *sh, size_t argc, char *argv[],
			    gapm_le_adv_create_param_t *param)
{
	if (argc < 2) {
		shell_error(sh, "Specify advertising type");
		return false;
	}

	/* Set default values */
	param->prop = GAPM_ADV_PROP_UNDIR_CONN_MASK, param->disc_mode = GAPM_ADV_MODE_GEN_DISC,
#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 /* ROM version > 1.0 */
	param->tx_pwr = 0,
#else
	param->max_tx_pwr = 0,
#endif /* !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0 */
	param->filter_pol = GAPM_ADV_ALLOW_SCAN_ANY_CON_ANY,
	param->prim_cfg.adv_intv_max = 800; /* 500 ms */
	param->prim_cfg.adv_intv_min = 160; /* 100 ms */
	param->prim_cfg.ch_map = ADV_ALL_CHNLS_EN;
	param->prim_cfg.phy = GAPM_PHY_TYPE_LE_1M;

	if (!strcmp(argv[1], "conn-scan")) {
		param->prop = GAPM_ADV_PROP_UNDIR_CONN_MASK;
	} else if (!strcmp(argv[1], "conn-nscan")) {
		param->prop = GAPM_ADV_PROP_CONNECTABLE_BIT;
	} else if (!strcmp(argv[1], "nconn-scan")) {
		param->prop = GAPM_ADV_PROP_SCANNABLE_BIT;
	} else if (!strcmp(argv[1], "nconn-nscan")) {
		param->prop = GAPM_ADV_PROP_NON_CONN_NON_SCAN_MASK;
	} else {
		shell_error(sh, "Provide a valid advertising type");
		return false;
	}

	for (size_t argn = 2; argn < argc; argn++) {
		const char *arg = argv[argn];

		if (!strcmp(arg, "disable-37")) {
			param->prim_cfg.ch_map &= ~ADV_CHNL_37_EN;
		} else if (!strcmp(arg, "disable-38")) {
			param->prim_cfg.ch_map &= ~ADV_CHNL_38_EN;
		} else if (!strcmp(arg, "disable-39")) {
			param->prim_cfg.ch_map &= ~ADV_CHNL_39_EN;
		} else {
			shell_error(sh, "Provide valid advertising options");
			return false;
		}
	}

	return true;
}

static int cmd_adv_create(const struct shell *sh, size_t argc, char *argv[])
{
	gapm_le_adv_create_param_t param;
	int err;

	if (!is_initialized(sh)) {
		return -ENOEXEC;
	}

	if (!adv_param_parse(sh, argc, argv, &param)) {
		shell_help(sh);
		return -ENOEXEC;
	}

	/* Initialize the advertising module */
	err = bt_adv_data_init();
	if (err) {
		LOG_ERR("Cannot initialize advertising module, error code: %u", err);
		__ASSERT(false, "Cannot initialize advertising module, error code: %u", err);
		return -ECANCELED;
	}

	int lock_ret = alif_ble_mutex_lock(K_MSEC(BLE_MUTEX_TIMEOUT_MS));

	if (lock_ret) {
		__ASSERT(false, "BLE mutex lock timeout");
		shell_error(sh, "BLE mutex lock timeout");
		return lock_ret;
	}
	err = gapm_le_create_adv_legacy(0, GAPM_STATIC_ADDR, &param, &le_adv_cbs);
	alif_ble_mutex_unlock();

	if (!err) {
		memcpy(&stored_adv.param, &param, sizeof(gapm_le_adv_create_param_t));
		stored_adv.valid = true;
	} else {
		shell_error(sh, "Failed to create advertiser set (0x%02x)", err);
	}

	return err;
}

static int cmd_adv_param(const struct shell *sh, size_t argc, char *argv[])
{
	gapm_le_adv_create_param_t param;
	int err;

	if (!is_initialized(sh)) {
		return -ENOEXEC;
	}

	if (!stored_adv.valid) {
		shell_error(
			sh,
			"Initialize advertising parameters first. Run 'bt adv-create' command.");
		return -EINVAL;
	}

	if (argc < 2) {
		/* Display current parameters */
		shell_print(sh, "Current advertising parameters:");
		shell_print(sh, "  Type: %s",
			    (stored_adv.param.prop == GAPM_ADV_PROP_UNDIR_CONN_MASK) ? "conn-scan"
			    : (stored_adv.param.prop == GAPM_ADV_PROP_CONNECTABLE_BIT)
				    ? "conn-nscan"
			    : (stored_adv.param.prop == GAPM_ADV_PROP_SCANNABLE_BIT) ? "nconn-scan"
			    : (stored_adv.param.prop == GAPM_ADV_PROP_NON_CONN_NON_SCAN_MASK)
				    ? "nconn-nscan"
				    : "unknown");
		shell_print(sh, "  Interval: min %u ms, max %u ms",
			    (stored_adv.param.prim_cfg.adv_intv_min * 625) / 1000,
			    (stored_adv.param.prim_cfg.adv_intv_max * 625) / 1000);
		shell_print(sh, "  Channels: %s %s %s",
			    (stored_adv.param.prim_cfg.ch_map & ADV_CHNL_37_EN) ? "37 " : "",
			    (stored_adv.param.prim_cfg.ch_map & ADV_CHNL_38_EN) ? "38 " : "",
			    (stored_adv.param.prim_cfg.ch_map & ADV_CHNL_39_EN) ? "39" : "");
		return 0;
	}

	/* Start with stored parameters */
	memcpy(&param, &stored_adv.param, sizeof(gapm_le_adv_create_param_t));

	/* Check for individual parameter updates */
	for (size_t argn = 1; argn < argc; argn++) {
		const char *arg = argv[argn];

		if (!strcmp(argv[argn], "conn-scan")) {
			param.prop = GAPM_ADV_PROP_UNDIR_CONN_MASK;
		} else if (!strcmp(argv[argn], "conn-nscan")) {
			param.prop = GAPM_ADV_PROP_CONNECTABLE_BIT;
		} else if (!strcmp(argv[argn], "nconn-scan")) {
			param.prop = GAPM_ADV_PROP_SCANNABLE_BIT;
		} else if (!strcmp(argv[argn], "nconn-nscan")) {
			param.prop = GAPM_ADV_PROP_NON_CONN_NON_SCAN_MASK;
		} else if (!strcmp(arg, "interval-min")) {
			if (++argn == argc) {
				shell_error(sh, "Specify interval value in milliseconds");
				return -EINVAL;
			}

			uint32_t interval_ms = strtoul(argv[argn], NULL, 10);
			/* Convert from ms to 0.625ms units */
			param.prim_cfg.adv_intv_min = (interval_ms * 1000) / 625;
			shell_print(sh, "Set minimum advertising interval to %u ms (%u units)",
				    interval_ms, param.prim_cfg.adv_intv_min);
		} else if (!strcmp(arg, "interval-max")) {
			if (++argn == argc) {
				shell_error(sh, "Specify interval value in milliseconds");
				return -EINVAL;
			}

			uint32_t interval_ms = strtoul(argv[argn], NULL, 10);
			/* Convert from ms to 0.625ms units */
			param.prim_cfg.adv_intv_max = (interval_ms * 1000) / 625;
			shell_print(sh, "Set maximum advertising interval to %u ms (%u units)",
				    interval_ms, param.prim_cfg.adv_intv_max);
		} else if (!strcmp(arg, "disable-37")) {
			param.prim_cfg.ch_map &= ~ADV_CHNL_37_EN;
			shell_print(sh, "Disabled advertising on channel 37");
		} else if (!strcmp(arg, "enable-37")) {
			param.prim_cfg.ch_map |= ADV_CHNL_37_EN;
			shell_print(sh, "Enabled advertising on channel 37");
		} else if (!strcmp(arg, "disable-38")) {
			param.prim_cfg.ch_map &= ~ADV_CHNL_38_EN;
			shell_print(sh, "Disabled advertising on channel 38");
		} else if (!strcmp(arg, "enable-38")) {
			param.prim_cfg.ch_map |= ADV_CHNL_38_EN;
			shell_print(sh, "Enabled advertising on channel 38");
		} else if (!strcmp(arg, "disable-39")) {
			param.prim_cfg.ch_map &= ~ADV_CHNL_39_EN;
			shell_print(sh, "Disabled advertising on channel 39");
		} else if (!strcmp(arg, "enable-39")) {
			param.prim_cfg.ch_map |= ADV_CHNL_39_EN;
			shell_print(sh, "Enabled advertising on channel 39");
		} else {
			shell_error(sh, "Unrecognized parameter: %s", arg);
			return -EINVAL;
		}
	}

	k_sem_reset(&bt_process_sem);

	/* Recreate advertising with updated parameters */
	int lock_ret = alif_ble_mutex_lock(K_MSEC(BLE_MUTEX_TIMEOUT_MS));

	if (lock_ret) {
		__ASSERT(false, "BLE mutex lock timeout");
		shell_error(sh, "BLE mutex lock timeout");
		return lock_ret;
	}
	err = gapm_delete_activity(stored_adv.actv_idx);
	alif_ble_mutex_unlock();
	if (err) {
		shell_error(sh, "Cannot delete existing advertising set, error code: 0x%02x", err);
		return err;
	}

	err = k_sem_take(&bt_process_sem, K_SECONDS(10));
	if (err < 0) {
		shell_error(sh, "BLE stack not responding within timeout period");
		return err;
	}

	k_sem_reset(&bt_process_sem);

	lock_ret = alif_ble_mutex_lock(K_MSEC(BLE_MUTEX_TIMEOUT_MS));

	if (lock_ret) {
		__ASSERT(false, "BLE mutex lock timeout");
		shell_error(sh, "BLE mutex lock timeout");
		return lock_ret;
	}
	err = gapm_le_create_adv_legacy(0, GAPM_STATIC_ADDR, &param, &le_adv_cbs);
	alif_ble_mutex_unlock();
	if (err) {
		shell_error(sh, "Cannot modify advertising set, error code: 0x%02x", err);
		return err;
	}

	err = k_sem_take(&bt_process_sem, K_SECONDS(10));
	if (err < 0) {
		shell_error(sh, "BLE stack not responding within timeout period");
		return err;
	}

	/* Store updated parameters */
	memcpy(&stored_adv.param, &param, sizeof(gapm_le_adv_create_param_t));

	return 0;
}

static int cmd_adv_start(const struct shell *sh, size_t argc, char *argv[])
{
	if (!is_initialized(sh)) {
		return -ENOEXEC;
	}

	uint8_t num_events = 0;
	int32_t timeout = 0;
	int err;

	if (stored_adv.actv_idx == 0xFF) {
		shell_error(sh, "No advertising set created. Run 'bt adv-create' first.");
		return -EINVAL;
	}

	for (size_t argn = 1; argn < argc; argn++) {
		const char *arg = argv[argn];

		if (!strcmp(arg, "timeout")) {
			if (++argn == argc) {
				goto fail_show_help;
			}

			timeout = strtoul(argv[argn], NULL, 16);
			shell_print(sh, "Set advertising timeout to %d ms", timeout);
		}

		if (!strcmp(arg, "num-events")) {
			if (++argn == argc) {
				goto fail_show_help;
			}

			num_events = strtoul(argv[argn], NULL, 16);
			shell_print(sh, "Set advertising maximum events to %d", num_events);
		}
	}

	gapm_le_adv_param_t adv_params = {
		/* Advertise indefinitely by default */
		.duration = timeout,
		.max_adv_evt = num_events,
	};

	k_sem_reset(&bt_process_sem);

	int lock_ret = alif_ble_mutex_lock(K_MSEC(BLE_MUTEX_TIMEOUT_MS));

	if (lock_ret) {
		__ASSERT(false, "BLE mutex lock timeout");
		shell_error(sh, "BLE mutex lock timeout");
		return lock_ret;
	}
	err = gapm_le_start_adv(stored_adv.actv_idx, &adv_params);
	alif_ble_mutex_unlock();
	if (err) {
		shell_error(sh, "Cannot start LE advertising, error code: 0x%02x", err);
	} else {
		shell_print(sh, "Started advertising with activity index %d", stored_adv.actv_idx);
		if (timeout > 0) {
			shell_print(sh, "Advertising will stop after %d ms", timeout);
		}
		if (num_events > 0) {
			shell_print(sh, "Advertising will stop after %d events", num_events);
		}
	}

	err = k_sem_take(&bt_process_sem, K_SECONDS(10));
	if (err < 0) {
		shell_error(sh, "No response from the stack");
		return err;
	}

	return err;

fail_show_help:
	shell_help(sh);
	return -ENOEXEC;
}

static int cmd_adv_stop(const struct shell *sh, size_t argc, char *argv[])
{
	int err;

	if (!is_initialized(sh)) {
		return -ENETDOWN;
	}

	if (stored_adv.actv_idx == 0xFF) {
		shell_error(sh, "No advertising activity to stop");
		return -EINVAL;
	}

	k_sem_reset(&bt_process_sem);

	int lock_ret = alif_ble_mutex_lock(K_MSEC(BLE_MUTEX_TIMEOUT_MS));

	if (lock_ret) {
		__ASSERT(false, "BLE mutex lock timeout");
		shell_error(sh, "BLE mutex lock timeout");
		return lock_ret;
	}
	err = gapm_stop_activity(stored_adv.actv_idx);
	alif_ble_mutex_unlock();

	if (err) {
		shell_error(sh, "Cannot stop advertising, error code: 0x%02x", err);
		return err;
	}

	err = k_sem_take(&bt_process_sem, K_SECONDS(10));
	if (err < 0) {
		shell_error(sh, "No response from the stack");
		return err;
	}

	shell_print(sh, "Stopped advertising with activity index %d", stored_adv.actv_idx);
	return 0;
}

static int cmd_adv_delete(const struct shell *sh, size_t argc, char *argv[])
{
	int err;
	uint8_t preserved_actv_idx = stored_adv.actv_idx;

	if (!is_initialized(sh)) {
		return -ENETDOWN;
	}

	if (stored_adv.actv_idx == 0xFF) {
		shell_error(sh, "No advertising activity to delete");
		return -EINVAL;
	}

	k_sem_reset(&bt_process_sem);

	int lock_ret = alif_ble_mutex_lock(K_MSEC(BLE_MUTEX_TIMEOUT_MS));

	if (lock_ret) {
		__ASSERT(false, "BLE mutex lock timeout");
		shell_error(sh, "BLE mutex lock timeout");
		return lock_ret;
	}
	err = gapm_delete_activity(stored_adv.actv_idx);
	alif_ble_mutex_unlock();
	if (err) {
		shell_error(sh, "Cannot delete advertising, error code: 0x%02x", err);
		return err;
	}

	err = k_sem_take(&bt_process_sem, K_SECONDS(10));
	if (err < 0) {
		shell_error(sh, "No response from the stack");
		return err;
	}

	shell_print(sh, "Deleted advertising with activity index %d", preserved_actv_idx);
	stored_adv.valid = false;

	return 0;
}

static int cmd_default_handler(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 1) {
		shell_help(sh);
		return SHELL_CMD_HELP_PRINTED;
	}

	shell_error(sh, "%s unknown parameter: %s", argv[0], argv[1]);

	return -EINVAL;
}

static int cmd_adv_data(const struct shell *sh, size_t argc, char *argv[])
{
	int err;

	if (!is_initialized(sh)) {
		return -ENOEXEC;
	}

	if (stored_adv.actv_idx == 0xFF) {
		shell_error(sh, "No advertising set created. Run 'bt adv-create' first.");
		return -EINVAL;
	}

	/* Display current advertising data if no parameters */
	if (argc < 2) {
		uint8_t adv_data_len = bt_adv_data_get_length();

		if (adv_data_len > 0) {
			shell_print(sh, "Current advertising data: %u bytes", adv_data_len);

			/* Get the advertising data for hexdump */
			uint8_t *adv_data = bt_adv_data_get_raw();

			if (adv_data != NULL) {
				shell_hexdump(sh, adv_data, adv_data_len);
			}
		} else {
			shell_print(sh, "No advertising data set");
		}
		return 0;
	}

	/* Handle name command */
	if (!strcmp(argv[1], "name")) {
		if (argc < 3) {
			/* TODO: Extended advertising would support longer names
			 * Display current name if set
			 * Max data size: 31 - 2 (length, type)
			 */
			char name[CONFIG_BLE_DEVICE_NAME_MAX - 2];

			err = bt_adv_data_check_name(name, sizeof(name));
			if (err >= 0) {
				shell_print(sh, "Current name: %s", name);
				return 0;
			} else if (err == -ENOENT) {
				shell_print(sh, "No name set in advertising data");
				return 0;
			}

			shell_error(sh, "Failed to get name: %d", err);
			return err;
		}

		/* Set new name */
		const char *name = argv[2];
		size_t name_len = strlen(name);

		err = bt_adv_data_set_name_auto(stored_adv.actv_idx, name, name_len);
		if (err) {
			shell_error(sh, "Failed to set advertising name: %d", err);
			return err;
		}

		shell_print(sh, "Set advertising name to '%s'", name);
		return 0;
	} else if (!strcmp(argv[1], "manufacturer")) {
		if (argc < 3) {
			shell_print(sh,
				    "Usage: adv-data manufacturer <company_id> [data_bytes...]");
			return -EINVAL;
		}

		/* Parse company ID */
		uint16_t company_id;

		if (argv[2][0] == '0' && (argv[2][1] == 'x' || argv[2][1] == 'X')) {
			company_id = strtoul(argv[2], NULL, 16);
		} else {
			company_id = strtoul(argv[2], NULL, 10);
		}

		/* Parse data bytes
		 * Max data size: 31 - 2 (length, type) - 2 (company ID)
		 */
		uint8_t data[CONFIG_BLE_ADV_DATA_MAX - 2 - 2];
		size_t data_len = 0;

		for (int i = 3; i < argc && data_len < sizeof(data); i++) {
			char *arg = argv[i];
			unsigned long val;

			if (arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X')) {
				val = strtoul(arg, NULL, 16);
			} else {
				val = strtoul(arg, NULL, 10);
			}

			if (val > 0xFF) {
				shell_error(sh, "Invalid byte value: %s", arg);
				return -EINVAL;
			}

			data[data_len++] = (uint8_t)val;
		}

		err = bt_adv_data_set_manufacturer(stored_adv.actv_idx, company_id, data, data_len);
		if (err) {
			shell_error(sh, "Failed to set manufacturer data: %d", err);
			return err;
		}

		shell_print(sh, "Set manufacturer data for company ID 0x%04x (%u bytes)",
			    company_id, data_len);
		return 0;
	} else if (!strcmp(argv[1], "clear")) {
		err = bt_adv_data_clear(stored_adv.actv_idx);
		if (err) {
			shell_error(sh, "Failed to clear advertising data: %d", err);
			return err;
		}

		shell_print(sh, "Cleared all advertising data");
		return 0;
	}

	shell_error(sh, "Unknown parameter: %s", argv[1]);
	return -EINVAL;
}

#define HELP_NONE "[none]"
#define HELP_ADV_CREATE                                                                            \
	"<conn-scan | conn-nscan | nconn-scan | nconn-nscan> "                                     \
	"[disable-37] [disable-38] [disable-39]"
#define HELP_ADV_PARAM_OPT                                                                         \
	"[disable-37] [disable-38] [disable-39] "                                                  \
	"[enable-37] [enable-38] [enable-39]"

#define HELP_ADV_DATA                                                                              \
	"[name <device_name>] [manufacturer <manuf_data>] [service-data <service_data>]"

SHELL_STATIC_SUBCMD_SET_CREATE(
	bt_cmds, SHELL_CMD_ARG(init, NULL, "[no-settings-load] [sync]", cmd_init, 1, 2),

	SHELL_CMD_ARG(adv-create, NULL, HELP_ADV_CREATE, cmd_adv_create, 2, 3),
	SHELL_CMD_ARG(adv-param, NULL, HELP_ADV_PARAM_OPT, cmd_adv_param, 0, 4),
	SHELL_CMD_ARG(adv-data, NULL, HELP_ADV_DATA, cmd_adv_data, 0, 4),
	SHELL_CMD_ARG(adv-start, NULL, "[timeout <timeout>] [num-events <num events>]",
		      cmd_adv_start, 0, 4),
	SHELL_CMD_ARG(adv-stop, NULL, HELP_NONE, cmd_adv_stop, 0, 0),
	SHELL_CMD_ARG(adv-delete, NULL, HELP_NONE, cmd_adv_delete, 0, 0),

	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(bt, &bt_cmds, "Bluetooth shell commands", cmd_default_handler);
