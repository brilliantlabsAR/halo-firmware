/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <string.h>

#include "halo/ble_connection.h"
#include "halo/ble_internal.h"
#include "halo/ble_security.h"
#include "halo/ble_service.h"
#include "halo/ble_lua.h"
#include "halo/pm_manager.h"
#ifdef CONFIG_HALO_BLE_AUDIO_SERVICE
#include "halo/ble_audio.h"
#endif

/* SE Service for EUI48 */
#include "se_service.h"

/* Alif BLE includes */
#include "alif_ble.h"
#include "gapm.h"
#include "gapm_le_init.h"
#include "gapm_le_adv.h"
#include "gap_le.h"
#include "gapc.h"
#include "gapc_le.h"
#include "gapc_sec.h"
#include "gatt_db.h"
#include "gatt_srv.h"
#include "co_buf.h"

LOG_MODULE_REGISTER(halo_ble_conn, CONFIG_HALO_LOG_LEVEL);

/* Static address must start 0xC0 ~ 0xFF */
#ifdef CONFIG_MCUBOOT
#define FRAME_STATIC_ADDRESS (uint32_t)0xFF5994
#else
#define FRAME_STATIC_ADDRESS (uint32_t)0xFE5994
#endif

#define BLE_CONNECTION_INIT_MAGIC 0x424C4554 /* 'BLET' */
#define CONN_INT_MIN_SLOTS        24
#define CONN_INT_MAX_SLOTS        40
#define CONN_TIMEOUT_SLOTS        1000 // 1 seconds

#define APPEARANCE 0x0942

/* Generate EUI48 address using SE service */
static void alif_eui48_read(uint8_t *eui48)
{
#ifdef FRAME_STATIC_ADDRESS
	eui48[0] = (uint8_t)(FRAME_STATIC_ADDRESS >> 16);
	eui48[1] = (uint8_t)(FRAME_STATIC_ADDRESS >> 8);
	eui48[2] = (uint8_t)(FRAME_STATIC_ADDRESS);
#else
	se_service_get_rnd_num(&eui48[0], 3);
	eui48[0] |= 0xC0;
#endif
	se_system_get_eui_extension(true, &eui48[3]);
	if (eui48[3] || eui48[4] || eui48[5]) {
		return;
	}
	/* Generate Random Local value (ELI) */
	eui48[3] = 0x11;
	eui48[4] = 0x22;
	eui48[5] = 0x33;
}

/* Connection context */
static struct {
	uint32_t initialized;
	bool connected;
	bool advertising;
	/* Set while halo_ble_conn_deinit() runs: suppresses the automatic
	 * advertising restart in the disconnect handler, which otherwise
	 * races alif_ble_disable() and leaves the stack half-alive. */
	bool shutting_down;
	uint8_t conidx;
	uint8_t adv_idx;
	uint16_t mtu;
	uint32_t last_activity;
	char device_name[32];
	uint8_t device_addr[6];
	struct k_mutex lock;
	struct k_sem gapm_sem;
} conn_ctx __attribute__((noinit));

/* Lives in .bss (zeroed every boot), unlike conn_ctx above. Distinguishes
 * "initialized earlier THIS boot" from stale noinit magic left by a previous
 * boot — e.g. a deep-sleep teardown whose alif_ble_disable() failed, which
 * otherwise leaves initialized == MAGIC forever and makes every later
 * halo_ble_conn_init() a silent no-op: device up, BLE dead, no advertising. */
static bool conn_ctx_valid_this_boot;

const gapc_le_con_param_nego_with_ce_len_t preferred_connection_param = {
	.ce_len_min = 5,
	.ce_len_max = 10,
	.hdr.interval_min = CONN_INT_MIN_SLOTS,
	.hdr.interval_max = CONN_INT_MAX_SLOTS,
	.hdr.latency = 0,
	.hdr.sup_to = CONN_TIMEOUT_SLOTS};

/* Forward declarations for Alif BLE callbacks */
static void on_gapm_process_complete(uint32_t const metainfo, uint16_t const status);
static void on_adv_actv_stopped(uint32_t metainfo, uint8_t actv_idx, uint16_t reason);
static void on_adv_actv_proc_cmp(uint32_t metainfo, uint8_t proc_id, uint8_t actv_idx,
				 uint16_t status);
static void on_adv_created(uint32_t metainfo, uint8_t actv_idx, int8_t tx_pwr);
static void on_le_connection_req(uint8_t const conidx, uint32_t const metainfo,
				 uint8_t const actv_idx, uint8_t const role,
				 const gap_bdaddr_t *const p_peer_addr,
				 const gapc_le_con_param_t *const p_con_params,
				 uint8_t const clk_accuracy);
static void on_disconnection(uint8_t const conidx, uint32_t const metainfo, uint16_t const reason);
static void on_bond_data_updated(uint8_t const conidx, uint32_t const metainfo,
				 const gapc_bond_data_updated_t *const p_data);
static void on_name_get(uint8_t const conidx, uint32_t const metainfo, uint16_t const token,
			uint16_t const offset, uint16_t const max_len);
static void on_appearance_get(uint8_t const conidx, uint32_t const metainfo, uint16_t const token);
static void on_pref_param_get(uint8_t conidx, uint32_t metainfo, uint16_t token);

/* GAPM error callback */
#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0
static void on_gapm_err(uint32_t metainfo, uint8_t code)
{
	LOG_ERR("GAPM error: code=%u", code);
}

static const gapm_cb_t gapm_err_cbs = {
	.cb_hw_error = on_gapm_err,
};
#else
static void on_gapm_err(enum co_error err)
{
	LOG_ERR("GAPM error: %d", err);
}

static const gapm_err_info_config_cb_t gapm_err_cbs = {
	.ctrl_hw_error = on_gapm_err,
};
#endif

/* Connection request callbacks */
static const struct gapc_connection_req_cb gapc_con_cbs = {
	.le_connection_req = on_le_connection_req,
};

/* Connection info callbacks */
static const gapc_connection_info_cb_t gapc_inf_cbs = {
	.disconnected = on_disconnection,
	.bond_data_updated = on_bond_data_updated,
	.name_get = on_name_get,
	.appearance_get = on_appearance_get,
	.slave_pref_param_get = on_pref_param_get,
	/* Other callbacks in this struct are optional */
};

/* Forward declarations for security callbacks */
static void on_gapc_le_encrypt_req(uint8_t const conidx, uint32_t const metainfo,
				   uint16_t const ediv, const gap_le_random_nb_t *const p_rand);
static void on_gapc_sec_auth_info(uint8_t const conidx, uint32_t const metainfo,
				  uint8_t const sec_lvl, bool const encrypted,
				  uint8_t const key_size);
static void on_gapc_pairing_req(uint8_t const conidx, uint32_t const metainfo,
				uint8_t const auth_level);
static void on_gapc_pairing_succeed(uint8_t const conidx, uint32_t const metainfo,
				    uint8_t const pairing_level, bool const enc_key_present,
				    uint8_t const key_type);
static void on_gapc_pairing_failed(uint8_t const conidx, uint32_t const metainfo,
				   uint16_t const reason);
static void on_gapc_info_req(uint8_t const conidx, uint32_t const metainfo, uint8_t const exp_info);
static void on_ltk_req(uint8_t const conidx, uint32_t const metainfo, uint8_t const key_size);
static void on_gapc_sec_numeric_compare_req(uint8_t const conidx, uint32_t const metainfo,
					    uint32_t const value);
static void on_key_received(uint8_t const conidx, uint32_t const metainfo,
			    const gapc_pairing_keys_t *const p_keys);

/* Security callbacks */
static const gapc_security_cb_t gapc_sec_cbs = {
	.le_encrypt_req = on_gapc_le_encrypt_req,
	.auth_info = on_gapc_sec_auth_info,
	.pairing_succeed = on_gapc_pairing_succeed,
	.pairing_failed = on_gapc_pairing_failed,
	.info_req = on_gapc_info_req,
	.ltk_req = on_ltk_req,
	.pairing_req = on_gapc_pairing_req,
	.numeric_compare_req = on_gapc_sec_numeric_compare_req,
	.key_received = on_key_received,
	/* Others are useless for LE peripheral device */
};

/* Forward declarations for LE config callbacks */
static void on_param_update_req(uint8_t conidx, uint32_t metainfo,
				const gapc_le_con_param_nego_t *p_param);
static void on_param_updated(uint8_t conidx, uint32_t metainfo, const gapc_le_con_param_t *p_param);
static void on_packet_size_updated(uint8_t conidx, uint32_t metainfo, uint16_t max_tx_octets,
				   uint16_t max_tx_time, uint16_t max_rx_octets,
				   uint16_t max_rx_time);
static void on_phy_updated(uint8_t conidx, uint32_t metainfo, uint8_t tx_phy, uint8_t rx_phy);
static void on_subrate_updated(uint8_t conidx, uint32_t metainfo,
			       const gapc_le_subrate_t *p_subrate_params);

/* LE config callbacks */
static const gapc_le_config_cb_t gapc_le_cfg_cbs = {
	.param_update_req = on_param_update_req,
	.param_updated = on_param_updated,
	.packet_size_updated = on_packet_size_updated,
	.phy_updated = on_phy_updated,
	.subrate_updated = on_subrate_updated,
};

/* GAPM callbacks structure */
static const gapm_callbacks_t gapm_cbs = {
	.p_con_req_cbs = &gapc_con_cbs,
	.p_sec_cbs = &gapc_sec_cbs,
	.p_info_cbs = &gapc_inf_cbs,
	.p_le_config_cbs = &gapc_le_cfg_cbs,
	.p_bt_config_cbs = NULL,
#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0
	.p_gapm_cbs = &gapm_err_cbs,
#else
	.p_err_info_config_cbs = &gapm_err_cbs,
#endif
};

static void on_tx_change_report(uint8_t conidx, uint32_t metainfo, bool local,
				const gapc_le_tx_power_report_t *p_report);

static const gapc_le_power_cb_t le_pwr_cb = {.tx_change_report = &on_tx_change_report};

/* Alif BLE advertising callbacks */
static const gapm_le_adv_cb_actv_t le_adv_cbs = {
	.hdr.actv.stopped = on_adv_actv_stopped,
	.hdr.actv.proc_cmp = on_adv_actv_proc_cmp,
	.created = on_adv_created,
};

int halo_ble_conn_init(const char *device_name)
{
	int err;

	if (conn_ctx.initialized == BLE_CONNECTION_INIT_MAGIC) {
		if (conn_ctx_valid_this_boot) {
			return 0;
		}
		/* Stale noinit state from a previous boot: reinitialize fully. */
		LOG_WRN("Discarding stale BLE connection state from a previous boot");
		conn_ctx.initialized = 0;
	}

	k_mutex_init(&conn_ctx.lock);
	k_sem_init(&conn_ctx.gapm_sem, 0, 1);
	conn_ctx.last_activity = k_uptime_get_32();

	/* Enable Alif BLE stack */
	err = alif_ble_enable(NULL);
	if (err == -EALREADY) {
		LOG_DBG("Alif BLE already enabled");
		LOG_INF("BLE device name: %s", conn_ctx.device_name);
		/* A prior deinit set this to suppress advertising restarts;
		 * clear it or no disconnect will ever restart advertising. */
		conn_ctx.shutting_down = false;
		conn_ctx.initialized = BLE_CONNECTION_INIT_MAGIC;
		conn_ctx_valid_this_boot = true;
		return err;
	} else if (err != 0) {
		LOG_ERR("alif_ble_enable failed: %d", err);
		return err;
	}

	/* Initialize connection context for first time */
	conn_ctx.connected = false;
	conn_ctx.advertising = false;
	conn_ctx.shutting_down = false;
	conn_ctx.last_activity = k_uptime_get_32();
	conn_ctx.conidx = GAP_INVALID_CONIDX;
	conn_ctx.adv_idx = 0;
	conn_ctx.mtu = 23; /* Default ATT MTU */
	conn_ctx.device_name[0] = '\0';
	memset(conn_ctx.device_addr, 0, sizeof(conn_ctx.device_addr));

	if (err == 0) {
		LOG_DBG("Configuring Alif BLE stack...");

		/* Get EUI48 address from SE service */
		uint8_t eui48[6];
		alif_eui48_read(eui48);

		/* Set device name: use parameter if provided, otherwise generate from EUI48 */
		if (device_name && device_name[0] != '\0') {
			strncpy(conn_ctx.device_name, device_name,
				sizeof(conn_ctx.device_name) - 1);
			conn_ctx.device_name[sizeof(conn_ctx.device_name) - 1] = '\0';
		} else {
			/* Generate device name based on EUI48 (matching le_main.c format) */
			snprintf(conn_ctx.device_name, sizeof(conn_ctx.device_name), "Halo %02X",
				 eui48[3]);
		}

		/* Configure GAPM - this is the actual Alif BLE initialization */
		static gapm_config_t gapm_cfg = {
			.role = GAP_ROLE_LE_PERIPHERAL,
			.pairing_mode = GAPM_PAIRING_MODE_ALL,
			.privacy_cfg = GAPM_PRIV_CFG_PRIV_ADDR_BIT,
			.renew_dur = 1500,
			.private_identity.addr = {0, 0, 0, 0, 0, 0},
			.irk.key = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x08, 0x11, 0x22,
				    0x33, 0x44, 0x55, 0x66, 0x77, 0x88},
			.gap_start_hdl = 0,
			.gatt_start_hdl = 0,
			.att_cfg = 0,
			.sugg_max_tx_octets = GAP_LE_MAX_OCTETS,
			.sugg_max_tx_time = GAP_LE_MAX_TIME,
			.tx_pref_phy = GAP_PHY_LE_2MBPS,
			.rx_pref_phy = GAP_PHY_LE_2MBPS,
			.tx_path_comp = 0,
			.rx_path_comp = 0,
			.class_of_device = 0,
			.dflt_link_policy = 0,
		};

		/* Set private_identity.addr from EUI48 with reversed byte order */
		for (int i = 0; i < 6; i++) {
			gapm_cfg.private_identity.addr[i] = eui48[5 - i];
		}

#ifdef MCU_BOOT
		gapm_cfg.private_identity.addr[4] = 58;
#endif

		/* Configure GAPM with callbacks - this uses the real Alif API */
		err = gapm_configure(0, &gapm_cfg, &gapm_cbs, on_gapm_process_complete);
		if (err != GAP_ERR_NO_ERROR) {
			LOG_ERR("gapm_configure failed: %u", err);
			return -EIO;
		}

		/* Wait for configuration to complete */
		k_sem_take(&conn_ctx.gapm_sem, K_FOREVER);

		/* Get device address */
		gap_bdaddr_t identity;
		gapm_get_identity(&identity);
		memcpy(conn_ctx.device_addr, identity.addr, 6);

		/* Set device name using Alif API */
		err = gapm_set_name(1, strlen(conn_ctx.device_name), conn_ctx.device_name,
				    on_gapm_process_complete);
		if (err != GAP_ERR_NO_ERROR) {
			LOG_ERR("gapm_set_name failed: %u", err);
			return -EIO;
		}

		gapm_le_set_appearance(APPEARANCE);

		/* Wait for name to be set */
		k_sem_take(&conn_ctx.gapm_sem, K_FOREVER);
	}

	gapc_le_power_set_callbacks(&le_pwr_cb);

	LOG_INF("BLE device name: %s", conn_ctx.device_name);

	LOG_DBG("BLE initialized with address %02X:%02X:%02X:%02X:%02X:%02X",
		conn_ctx.device_addr[5], conn_ctx.device_addr[4], conn_ctx.device_addr[3],
		conn_ctx.device_addr[2], conn_ctx.device_addr[1], conn_ctx.device_addr[0]);

	conn_ctx.initialized = BLE_CONNECTION_INIT_MAGIC;
	conn_ctx_valid_this_boot = true;

	return 0;
}

const char *halo_ble_conn_get_device_name(void)
{
	return conn_ctx.device_name;
}

/* GAPM process completion callback - called by Alif BLE stack */
static void on_gapm_process_complete(uint32_t const metainfo, uint16_t const status)
{
	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("GAPM process failed: metainfo=%u, status=%u", metainfo, status);
	}
	k_sem_give(&conn_ctx.gapm_sem);
}

int halo_ble_conn_adv_start(const struct halo_ble_adv_params *params)
{
	int err;
	gapm_le_adv_create_param_t adv_create_params;

	if (conn_ctx.initialized != BLE_CONNECTION_INIT_MAGIC) {
		return -ENODEV;
	}

	k_mutex_lock(&conn_ctx.lock, K_FOREVER);

	if (conn_ctx.advertising) {
		k_mutex_unlock(&conn_ctx.lock);
		LOG_DBG("Already advertising");
		return 0;
	}

	/* Set advertising parameters - using real Alif BLE API */
	adv_create_params.prop = GAPM_ADV_PROP_UNDIR_CONN_MASK;
	adv_create_params.disc_mode = GAPM_ADV_MODE_GEN_DISC;
	adv_create_params.filter_pol = GAPM_ADV_ALLOW_SCAN_ANY_CON_ANY;

#if !CONFIG_ALIF_BLE_ROM_IMAGE_V1_0
	adv_create_params.tx_pwr = 0;
#else
	adv_create_params.max_tx_pwr = 0;
#endif

	if (params) {
		adv_create_params.prim_cfg.adv_intv_min = params->interval_min;
		adv_create_params.prim_cfg.adv_intv_max = params->interval_max;
		adv_create_params.prim_cfg.ch_map = params->channel_map;
	} else {
		/* Default values */
		adv_create_params.prim_cfg.adv_intv_min = 24;  /* 15 ms */ 
		adv_create_params.prim_cfg.adv_intv_max = 40;  /* 25 ms */
		adv_create_params.prim_cfg.ch_map = ADV_ALL_CHNLS_EN;
	}
	adv_create_params.prim_cfg.phy = GAPM_PHY_TYPE_LE_1M;

	/* Create advertising activity - real Alif BLE call with simple retry */
	for (int i = 0; i < 3; i++) {
		err = gapm_le_create_adv_legacy(0, GAPM_STATIC_ADDR, &adv_create_params, &le_adv_cbs);
		if (err == GAP_ERR_NO_ERROR) {
			break;
		}
		if (i < 2) {
			k_msleep(10);
		}
	}

	if (err != GAP_ERR_NO_ERROR) {
		k_mutex_unlock(&conn_ctx.lock);
		LOG_ERR("gapm_le_create_adv_legacy failed: %u", err);
		return -EIO;
	}

	conn_ctx.advertising = true;

	k_mutex_unlock(&conn_ctx.lock);

	/* Notify event */
	struct halo_ble_event_data event = {
		.event = HALO_BLE_EVENT_ADV_STARTED,
		.conidx = GAP_INVALID_CONIDX,
	};
	halo_ble_notify_event(&event);

	return 0;
}

/* Helper function to set advertising data */
static uint16_t set_advertising_data(uint8_t actv_idx)
{
	const size_t device_name_len = strlen(conn_ctx.device_name);
	/* HALO service UUID (using same UUID as Frame's LUA service for compatibility) */
	static const uint8_t halo_srv_uuid[] = HALO_LUA_UUID_128_SVC;
	const uint16_t adv_len = device_name_len + GATT_HANDLE_LEN + GATT_UUID_128_LEN + GATT_HANDLE_LEN;
	co_buf_t *p_buf;
	uint16_t err = co_buf_alloc(&p_buf, 0, adv_len, 0);

	if (err != 0) {
		LOG_ERR("Buffer allocation failed: %u", err);
		return err;
	}

	uint8_t *p_data = co_buf_data(p_buf);

	/* NAME */
	p_data[0] = device_name_len + 1;
	p_data[1] = 0x09; /* Complete local name */
	memcpy(p_data + 2, conn_ctx.device_name, device_name_len);

	/* SERVICE DATA */
	p_data += device_name_len + 2;
	p_data[0] = (GATT_UUID_128_LEN + 1);
	p_data[1] = GAP_AD_TYPE_COMPLETE_LIST_128_BIT_UUID;
	memcpy(p_data + 2, halo_srv_uuid, GATT_UUID_128_LEN);

	err = gapm_le_set_adv_data(actv_idx, p_buf);
	co_buf_release(p_buf); /* Release ownership of buffer so stack can free it when done */

	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to set advertising data: %u", err);
	} else {
		LOG_DBG("Advertising data set successfully");
	}

	return err;
}

/* Legacy scan response payload limit */
#define SCAN_RSP_MAX_LEN 31

/* Helper function to set scan response data
 *
 * The 31-byte payload cannot hold everything when both ANCS and LE Audio are
 * enabled (base 8 + ANCS solicitation 18 + LE Audio announcements 21 = 47), so
 * entries are appended in priority order and dropped once space runs out:
 *   1. Appearance + Battery UUID (base, scanners filter on these)
 *   2. ANCS service solicitation (required for iOS to offer notification access)
 *   3. LE Audio announcements: ASCS > CAS > TMAS (discovery hints only - hosts
 *      read the real services over GATT after connecting)
 * With both features on, that yields base + ANCS + CAS (31 bytes exactly).
 */
static uint16_t set_scan_response_data(uint8_t actv_idx)
{
	uint8_t data[SCAN_RSP_MAX_LEN];
	uint16_t len = 0;

	/* Appearance: Eye-glasses (0x01C0), Little Endian */
	data[len++] = 3;    /* Length */
	data[len++] = 0x19; /* Type: Appearance */
	data[len++] = 0xC0; /* Value LSB */
	data[len++] = 0x01; /* Value MSB */

	/* Battery Service UUID (0x180F) */
	data[len++] = 3;    /* Length */
	data[len++] = 0x03; /* Type: Complete list of 16-bit Service UUIDs */
	data[len++] = 0x0F; /* LSB Battery Service UUID */
	data[len++] = 0x18; /* MSB Battery Service UUID */

#ifdef CONFIG_HALO_BLE_ANCS_SOLICIT_ADV
	/* ANCS Service Solicitation (7905F431-B5CE-4E99-A40F-4B1E122D00D0),
	 * LSB first - tells iOS the accessory wants notification access */
	static const uint8_t ancs_uuid[GATT_UUID_128_LEN] = {
		0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4,
		0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79};

	if (len + 2 + GATT_UUID_128_LEN <= sizeof(data)) {
		data[len++] = GATT_UUID_128_LEN + 1; /* Length */
		data[len++] = 0x15; /* Type: List of 128-bit Solicitation UUIDs */
		memcpy(&data[len], ancs_uuid, GATT_UUID_128_LEN);
		len += GATT_UUID_128_LEN;
	}
#endif

#ifdef CONFIG_HALO_BLE_AUDIO_SERVICE
	/* LE Audio announcements. The advertising PDU is full (name + 128-bit
	 * custom service UUID), so these ride in the scan response; Android
	 * merges ADV_IND + SCAN_RSP into a single scan record, which is what
	 * its LE Audio discovery filters operate on. */
	uint16_t sink_ctx = 0;
	uint16_t src_ctx = 0;

	halo_ble_audio_get_context_types(&sink_ctx, &src_ctx);

	/* ASCS (0x184E) Service Data: General Announcement + available contexts */
	if (len + 10 <= sizeof(data)) {
		data[len++] = 9;    /* Length */
		data[len++] = 0x16; /* Type: Service Data - 16-bit UUID */
		data[len++] = 0x4E; /* ASCS UUID LSB */
		data[len++] = 0x18; /* ASCS UUID MSB */
		data[len++] = 0x00; /* Announcement Type: General */
		data[len++] = (uint8_t)(sink_ctx & 0xFF); /* Available sink contexts */
		data[len++] = (uint8_t)((sink_ctx >> 8) & 0xFF);
		data[len++] = (uint8_t)(src_ctx & 0xFF); /* Available source contexts */
		data[len++] = (uint8_t)((src_ctx >> 8) & 0xFF);
		data[len++] = 0x00; /* Metadata length */
	}

	/* CAS (0x1853) Service Data: General Announcement (CAP Acceptor) */
	if (len + 5 <= sizeof(data)) {
		data[len++] = 4;    /* Length */
		data[len++] = 0x16; /* Type: Service Data - 16-bit UUID */
		data[len++] = 0x53; /* CAS UUID LSB */
		data[len++] = 0x18; /* CAS UUID MSB */
		data[len++] = 0x00; /* Announcement Type: General */
	}

	/* TMAS (0x1855) Service Data: TMAP Role */
	if (len + 6 <= sizeof(data)) {
		uint16_t tmap_roles = halo_ble_audio_get_tmap_roles();

		data[len++] = 5;    /* Length */
		data[len++] = 0x16; /* Type: Service Data - 16-bit UUID */
		data[len++] = 0x55; /* TMAS UUID LSB */
		data[len++] = 0x18; /* TMAS UUID MSB */
		data[len++] = (uint8_t)(tmap_roles & 0xFF);
		data[len++] = (uint8_t)((tmap_roles >> 8) & 0xFF);
	}
#endif /* CONFIG_HALO_BLE_AUDIO_SERVICE */

	co_buf_t *p_buf;
	uint16_t err = co_buf_alloc(&p_buf, 0, len, 0);

	if (err != 0) {
		LOG_ERR("Buffer allocation failed: %u", err);
		return err;
	}

	memcpy(co_buf_data(p_buf), data, len);

	err = gapm_le_set_scan_response_data(actv_idx, p_buf);
	co_buf_release(p_buf); /* Release ownership of buffer so stack can free it when done */

	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to set scan response data: %u", err);
	} else {
		LOG_DBG("Scan response data set successfully");
	}

	return err;
}

/* Helper function to start advertising */
static uint16_t start_advertising(uint8_t actv_idx)
{
	gapm_le_adv_param_t adv_params = {
		.duration = 0, /* Advertise indefinitely */
	};

	uint16_t err;
	for (int i = 0; i < 3; i++) {
		err = gapm_le_start_adv(actv_idx, &adv_params);
		if (err == GAP_ERR_NO_ERROR) {
			break;
		}
		if (i < 2) {
			k_msleep(10);
		}
	}

	if (err != GAP_ERR_NO_ERROR) {
		// reboot system if advertising fails
		LOG_ERR("Failed to start advertising: %u", err);
		halo_ble_conn_prepare_reboot();
		sys_reboot(SYS_REBOOT_COLD);
	} else {
		LOG_DBG("Advertising started successfully");
	}

	return err;
}

/* Advertising callbacks - called by Alif BLE stack */
static void on_adv_actv_stopped(uint32_t metainfo, uint8_t actv_idx, uint16_t reason)
{
	LOG_DBG("Advertising stopped: actv_idx=%u, reason=%u", actv_idx, reason);

	conn_ctx.advertising = false;

	struct halo_ble_event_data event = {
		.event = HALO_BLE_EVENT_ADV_STOPPED,
		.conidx = GAP_INVALID_CONIDX,
		.adv_stopped.reason = (uint8_t)reason,
	};
	halo_ble_notify_event(&event);
}

static void on_adv_actv_proc_cmp(uint32_t metainfo, uint8_t proc_id, uint8_t actv_idx,
				 uint16_t status)
{
	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Advertising process failed: proc_id=%u, status=%u", proc_id, status);
		return;
	}

	LOG_DBG("Advertising process complete: proc_id=%u, actv_idx=%u", proc_id, actv_idx);

	switch (proc_id) {
	case GAPM_ACTV_CREATE_LE_ADV:
		LOG_DBG("Advertising activity created");
		conn_ctx.adv_idx = actv_idx;
		/* Set advertising data */
		set_advertising_data(actv_idx);
		break;

	case GAPM_ACTV_SET_ADV_DATA:
		LOG_DBG("Advertising data set");
		/* Set scan response data */
		set_scan_response_data(actv_idx);
		break;

	case GAPM_ACTV_SET_SCAN_RSP_DATA:
		LOG_DBG("Scan response data set");
		/* Start advertising */
		start_advertising(actv_idx);
		break;

	case GAPM_ACTV_START:
		LOG_INF("Advertising started successfully");
		break;

	default:
		LOG_WRN("Unexpected GAPM activity complete: proc_id=%u", proc_id);
		break;
	}
}

static void on_adv_created(uint32_t metainfo, uint8_t actv_idx, int8_t tx_pwr)
{
	LOG_DBG("Advertising created: actv_idx=%u, tx_pwr=%d", actv_idx, tx_pwr);
}

int halo_ble_conn_adv_stop(void)
{
	int err;

	if (conn_ctx.initialized != BLE_CONNECTION_INIT_MAGIC) {
		return -ENODEV;
	}

	k_mutex_lock(&conn_ctx.lock, K_FOREVER);

	if (!conn_ctx.advertising) {
		k_mutex_unlock(&conn_ctx.lock);
		return 0;
	}

	/* Stop advertising using Alif BLE API */
	err = gapm_stop_activity(conn_ctx.adv_idx);
	if (err != GAP_ERR_NO_ERROR) {
		k_mutex_unlock(&conn_ctx.lock);
		LOG_ERR("gapm_stop_activity failed: %u", err);
		return -EIO;
	}

	conn_ctx.advertising = false;
	LOG_DBG("Advertising stopped");

	k_mutex_unlock(&conn_ctx.lock);

	return 0;
}

int halo_ble_conn_disconnect(void)
{
	uint8_t conidx;

	if (conn_ctx.initialized != BLE_CONNECTION_INIT_MAGIC) {
		return -ENODEV;
	}

	k_mutex_lock(&conn_ctx.lock, K_FOREVER);

	if (!conn_ctx.connected) {
		k_mutex_unlock(&conn_ctx.lock);
		return -ENOTCONN;
	}

	conidx = conn_ctx.conidx;
	k_mutex_unlock(&conn_ctx.lock);

	/* Disconnect using Alif BLE API */
	gapc_disconnect(conidx, 0, LL_ERR_REMOTE_USER_TERM_CON, NULL);

	LOG_INF("Disconnecting conidx %u", conidx);
	return 0;
}

int halo_ble_conn_deinit(bool disable)
{
	if (conn_ctx.initialized != BLE_CONNECTION_INIT_MAGIC) {
		return 0;
	}
	LOG_DBG("Deinitializing BLE connection manager");

	/* From here on the disconnect handler must not restart advertising:
	 * the async advertising state machine racing alif_ble_disable() left
	 * the stack half-alive with dense timers, which blocked SOFT_OFF. */
	conn_ctx.shutting_down = true;

	/* Tear down any live link first: alif_ble_disable() with an active
	 * connection never completes, which wedged the deep-sleep suspend
	 * chain whenever a peer was connected at shutdown. */
	if (halo_ble_conn_is_connected()) {
		int waited = 0;

		(void)halo_ble_conn_disconnect();
		while (halo_ble_conn_is_connected() && waited < 2000) {
			k_sleep(K_MSEC(20));
			waited += 20;
		}
		if (halo_ble_conn_is_connected()) {
			LOG_WRN("Peer did not disconnect within %d ms, disabling anyway",
				waited);
		} else {
			LOG_DBG("Disconnected peer before disable (%d ms)", waited);
		}
	}

	/* Disable Alif BLE stack. Mark the context uninitialized even when
	 * disable fails: leaving initialized == MAGIC in noinit RAM makes
	 * every later init a silent no-op (this boot and, worse, the next
	 * warm boot after deep sleep) - BLE stays dead with no advertising. */
	conn_ctx.initialized = 0;
	conn_ctx.advertising = false;
	conn_ctx.connected = false;

	if (disable) {
		int err = alif_ble_disable();
		if (err) {
			LOG_ERR("Failed to disable Alif BLE stack: %d", err);
			return err;
		}
	}

	return 0;
}

bool halo_ble_conn_is_connected(void)
{
	bool connected;

	if (conn_ctx.initialized != BLE_CONNECTION_INIT_MAGIC) {
		return false;
	}

	k_mutex_lock(&conn_ctx.lock, K_FOREVER);
	connected = conn_ctx.connected;
	k_mutex_unlock(&conn_ctx.lock);

	return connected;
}

bool halo_ble_conn_is_advertising(void)
{
	bool advertising;

	if (conn_ctx.initialized != BLE_CONNECTION_INIT_MAGIC) {
		return false;
	}

	k_mutex_lock(&conn_ctx.lock, K_FOREVER);
	advertising = conn_ctx.advertising;
	k_mutex_unlock(&conn_ctx.lock);

	return advertising;
}

uint8_t halo_ble_conn_get_conidx(void)
{
	uint8_t conidx;

	if (conn_ctx.initialized != BLE_CONNECTION_INIT_MAGIC) {
		return GAP_INVALID_CONIDX;
	}

	k_mutex_lock(&conn_ctx.lock, K_FOREVER);
	conidx = conn_ctx.conidx;
	k_mutex_unlock(&conn_ctx.lock);

	return conidx;
}

uint16_t halo_ble_conn_get_mtu(void)
{
	uint16_t mtu;

	if (conn_ctx.initialized != BLE_CONNECTION_INIT_MAGIC) {
		return 0;
	}

	if (!conn_ctx.connected) {
		return CFG_MAX_LE_MTU - GATT_BUFFER_HEADER_LEN; /* Default ATT MTU */
	}

	/* Get MTU from Alif BLE stack */
	alif_ble_mutex_lock(K_FOREVER);
	mtu = gatt_bearer_mtu_min_get(conn_ctx.conidx);
	alif_ble_mutex_unlock();

	return mtu > GATT_BUFFER_HEADER_LEN ? mtu - GATT_BUFFER_HEADER_LEN : 0;
}

int halo_ble_conn_get_address(uint8_t addr[6])
{
	if (!addr) {
		return -EINVAL;
	}

	if (conn_ctx.initialized != BLE_CONNECTION_INIT_MAGIC) {
		return -ENODEV;
	}

	k_mutex_lock(&conn_ctx.lock, K_FOREVER);
	memcpy(addr, conn_ctx.device_addr, 6);
	k_mutex_unlock(&conn_ctx.lock);

	return 0;
}

int halo_ble_conn_update_params(const struct halo_ble_conn_params *params)
{
	int err;

	if (!params) {
		return -EINVAL;
	}

	if (conn_ctx.initialized != BLE_CONNECTION_INIT_MAGIC) {
		return -ENODEV;
	}

	if (!conn_ctx.connected) {
		return -ENOTCONN;
	}

	/* Convert to Alif BLE format with CE length */
	gapc_le_con_param_nego_with_ce_len_t con_params_full = {
		.hdr =
			{
				.interval_min = params->interval_min,
				.interval_max = params->interval_max,
				.latency = params->latency,
				.sup_to = params->timeout,
			},
		.ce_len_min = 0,
		.ce_len_max = 0xFFFF,
	};

	/* Update connection parameters using Alif BLE API */
	err = gapc_le_update_params(conn_ctx.conidx, 0, &con_params_full, NULL);
	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("gapc_le_update_params failed: %u", err);
		return -EIO;
	}

	return 0;
}

void halo_ble_conn_update_activity(void)
{
	if (conn_ctx.initialized != BLE_CONNECTION_INIT_MAGIC) {
		return;
	}

	k_mutex_lock(&conn_ctx.lock, K_FOREVER);
	conn_ctx.last_activity = k_uptime_get_32();
	k_mutex_unlock(&conn_ctx.lock);
}

uint32_t halo_ble_conn_get_last_activity(void)
{
	uint32_t activity;

	if (conn_ctx.initialized != BLE_CONNECTION_INIT_MAGIC) {
		return 0;
	}

	k_mutex_lock(&conn_ctx.lock, K_FOREVER);
	activity = conn_ctx.last_activity;
	k_mutex_unlock(&conn_ctx.lock);

	return activity;
}

void le_get_tx_power_cmp_cb(uint8_t conidx, uint32_t metainfo, uint16_t status, uint8_t phy,
			    int8_t power_level, int8_t max_power_level)
{
	LOG_DBG("TX Power Level Report: conidx=%u, phy=%u, power_level=%d dBm, max_power_level=%d "
		"dBm",
		conidx, phy, power_level, max_power_level);
}

void le_set_tx_power_cmp_cb(uint8_t conidx, uint32_t metainfo, uint16_t status, int8_t new_tx_pwr)
{
	LOG_DBG("Set TX Power Level Complete: conidx=%u, new_tx_pwr=%d dBm", conidx, new_tx_pwr);
}

/**
 * @brief Handle connection event (called by Alif BLE callbacks)
 */
void halo_ble_conn_on_connected(uint8_t conidx, uint16_t mtu)
{
	if (conn_ctx.initialized != BLE_CONNECTION_INIT_MAGIC) {
		return;
	}

	k_mutex_lock(&conn_ctx.lock, K_FOREVER);

	conn_ctx.connected = true;
	conn_ctx.advertising = false;
	conn_ctx.conidx = conidx;
	conn_ctx.mtu = mtu;
	conn_ctx.last_activity = k_uptime_get_32();

	LOG_INF("Connected: conidx=%u, MTU=%u", conidx, mtu);

	k_mutex_unlock(&conn_ctx.lock);

	struct halo_ble_event_data event = {
		.event = HALO_BLE_EVENT_CONNECTED,
		.conidx = conidx,
	};
	halo_ble_notify_event(&event);
}

/**
 * @brief Handle disconnection event (called by Alif BLE callbacks)
 */
void halo_ble_conn_on_disconnected(uint8_t conidx, uint16_t reason)
{
	if (conn_ctx.initialized != BLE_CONNECTION_INIT_MAGIC) {
		return;
	}

	k_mutex_lock(&conn_ctx.lock, K_FOREVER);

	conn_ctx.connected = false;
	conn_ctx.conidx = GAP_INVALID_CONIDX;
	conn_ctx.mtu = 23;

	LOG_INF("Disconnected: conidx=%u, reason=0x%04x", conidx, reason);

	k_mutex_unlock(&conn_ctx.lock);

	/* Notify services first */
	halo_ble_service_on_disconnected(conidx);

	/* Then notify event callbacks */
	struct halo_ble_event_data event = {
		.event = HALO_BLE_EVENT_DISCONNECTED,
		.conidx = conidx,
		.disconnected.reason = reason,
	};
	halo_ble_notify_event(&event);

	/* Deinit in progress: leave the stack quiet so alif_ble_disable()
	 * does not race a restarting advertising state machine. */
	if (conn_ctx.shutting_down) {
		LOG_DBG("Shutdown in progress, not restarting advertising");
		return;
	}

	/* Calling set_advertising_data will trigger the state machine to set scan response and
	 * start advertising */
	uint16_t err = set_advertising_data(conn_ctx.adv_idx);
	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to restart advertising after disconnection: %u", err);
	} else {
		LOG_DBG("Advertising restarted after disconnection");
	}
}

/**
 * @brief Handle MTU change event (called by Alif BLE callbacks)
 */
void halo_ble_conn_on_mtu_changed(uint8_t conidx, uint16_t mtu)
{
	if (conn_ctx.initialized != BLE_CONNECTION_INIT_MAGIC) {
		return;
	}

	/* Get actual MTU from GATT bearer */
	alif_ble_mutex_lock(K_FOREVER);
	uint16_t actual_mtu = gatt_bearer_mtu_min_get(conidx);
	alif_ble_mutex_unlock();

	k_mutex_lock(&conn_ctx.lock, K_FOREVER);
	conn_ctx.mtu = actual_mtu;
	k_mutex_unlock(&conn_ctx.lock);

	/* Notify event */
	struct halo_ble_event_data event = {
		.event = HALO_BLE_EVENT_MTU_CHANGED,
		.conidx = conidx,
		.mtu_changed.mtu = actual_mtu,
	};

	halo_ble_notify_event(&event);
}

/**
 * @brief Connection request callback from Alif BLE stack
 */
static void on_le_connection_req(uint8_t const conidx, uint32_t const metainfo,
				 uint8_t const actv_idx, uint8_t const role,
				 const gap_bdaddr_t *const p_peer_addr,
				 const gapc_le_con_param_t *const p_con_params,
				 uint8_t const clk_accuracy)
{
	/* Prevent sleep during connection process */
	halo_pm_prevent_sleep();

	/* Update activity timestamp */
	halo_ble_conn_update_activity();

	/* Handle security check and connection confirmation via ble_security module */
	int err = halo_ble_sec_on_connection_req(conidx, p_peer_addr);
	if (err < 0) {
		LOG_ERR("Security check failed: %d", err);
		halo_pm_allow_sleep();
		return;
	}

	/* Update connection state */
	k_mutex_lock(&conn_ctx.lock, K_FOREVER);
	conn_ctx.connected = true;
	conn_ctx.conidx = conidx;
	k_mutex_unlock(&conn_ctx.lock);

	/* Notify services */
	halo_ble_service_on_connected(conidx);
	LOG_INF("Connected: conidx=%u", conidx);
	/* Notify event */
	struct halo_ble_event_data event = {
		.event = HALO_BLE_EVENT_CONNECTED,
		.conidx = conidx,
	};
	halo_ble_notify_event(&event);

	/* Allow sleep after connection process completes */
	halo_pm_allow_sleep();
}

/**
 * @brief Disconnection callback from Alif BLE stack
 */
static void on_disconnection(uint8_t const conidx, uint32_t const metainfo, uint16_t const reason)
{
	/* Call the internal disconnect handler */
	halo_ble_conn_on_disconnected(conidx, reason);
}

/**
 * @brief Encryption request callback from Alif BLE stack
 */
static void on_gapc_le_encrypt_req(uint8_t const conidx, uint32_t const metainfo,
				   uint16_t const ediv, const gap_le_random_nb_t *const p_rand)
{
	LOG_DBG("Encryption request: conidx=%u, ediv=0x%04x", conidx, ediv);
	/* Forward to security manager */
	int ret = halo_ble_sec_on_encrypt_req(conidx);
	if (ret < 0) {
		LOG_ERR("Failed to handle encryption request: %d", ret);
	}
}

/**
 * @brief Pairing request callback from Alif BLE stack
 */
static void on_gapc_pairing_req(uint8_t const conidx, uint32_t const metainfo,
				uint8_t const auth_level)
{
	/* Auto-accept pairing for now */
	halo_ble_sec_pair_accept(conidx);
}

/**
 * @brief Pairing success callback from Alif BLE stack
 */
static void on_gapc_pairing_succeed(uint8_t const conidx, uint32_t const metainfo,
				    uint8_t const pairing_level, bool const enc_key_present,
				    uint8_t const key_type)
{
	/* Notify security manager and event system */
	halo_ble_sec_on_paired(conidx, metainfo, pairing_level, enc_key_present, key_type);
}

/**
 * @brief Pairing failed callback from Alif BLE stack
 */
static void on_gapc_pairing_failed(uint8_t const conidx, uint32_t const metainfo,
				   uint16_t const reason)
{
	/* Notify security manager */
	halo_ble_sec_on_pair_failed(conidx, reason);
}

/**
 * @brief Key received callback from Alif BLE stack
 */
static void on_key_received(uint8_t const conidx, uint32_t const metainfo,
			    const gapc_pairing_keys_t *const p_keys)
{
	/* Forward to security manager for storage */
	halo_ble_sec_on_keys_received(conidx, p_keys);
}

/**
 * @brief Bond data updated callback from Alif BLE stack
 */
static void on_bond_data_updated(uint8_t const conidx, uint32_t const metainfo,
				 const gapc_bond_data_updated_t *const p_data)
{
	/* Bond data persistence handled by ble_security.c */
}

/**
 * @brief Name get callback from Alif BLE stack
 */
static void on_name_get(uint8_t const conidx, uint32_t const metainfo, uint16_t const token,
			uint16_t const offset, uint16_t const max_len)
{
	const size_t device_name_len = strlen(conn_ctx.device_name);
	const size_t short_len = (device_name_len > max_len ? max_len : device_name_len);

	gapc_le_get_name_cfm(conidx, token, GAP_ERR_NO_ERROR, device_name_len, short_len,
			     (const uint8_t *)conn_ctx.device_name);
}

/**
 * @brief Appearance get callback from Alif BLE stack
 */
static void on_appearance_get(uint8_t const conidx, uint32_t const metainfo, uint16_t const token)
{
	/* 0x01C3 = Headset */
	gapc_le_get_appearance_cfm(conidx, token, GAP_ERR_NO_ERROR, 0x01C3);
}

static void on_pref_param_get(uint8_t conidx, uint32_t metainfo, uint16_t token)
{

	gapc_le_preferred_periph_param_t prefs = {
		.con_intv_min = preferred_connection_param.hdr.interval_min,
		.con_intv_max = preferred_connection_param.hdr.interval_max,
		.latency = preferred_connection_param.hdr.latency,
		.conn_timeout = preferred_connection_param.hdr.sup_to,
	};

	gapc_le_get_preferred_periph_params_cfm(conidx, token, GAP_ERR_NO_ERROR, prefs);
}

/**
 * @brief Authentication info callback from Alif BLE stack
 */
static void on_gapc_sec_auth_info(uint8_t const conidx, uint32_t const metainfo,
				  uint8_t const sec_lvl, bool const encrypted,
				  uint8_t const key_size)
{
	LOG_DBG("Auth info: conidx=%u, sec_lvl=%u, encrypted=%d, key_size=%u", conidx, sec_lvl,
		encrypted, key_size);
	if (encrypted) {
		/* Authoritative "link encrypted" signal for both fresh pairing and
		 * bonded reconnection. */
		halo_ble_sec_on_encrypted(conidx, sec_lvl, key_size);
	} else if (halo_ble_sec_is_bonded()) {
		/* Encryption failed after being expected */
		LOG_WRN("Link not encrypted but bond exists - peer may have deleted keys");
		halo_ble_sec_on_encrypt_failed(conidx, 0x05); /* Authentication failure */
	}
}

/**
 * @brief Info request callback from Alif BLE stack
 */
static void on_gapc_info_req(uint8_t const conidx, uint32_t const metainfo, uint8_t const exp_info)
{
	uint16_t err;

	switch (exp_info) {
	case GAPC_INFO_IRK: {
		/* Forward to security manager to provide IRK */
		err = halo_ble_sec_provide_irk(conidx);
		if (err) {
			LOG_ERR("IRK provide failed: err=%u", err);
		}
		break;
	}
	case GAPC_INFO_CSRK: {
		/* Forward to security manager to provide CSRK */
		err = halo_ble_sec_provide_csrk(conidx);
		if (err) {
			LOG_ERR("CSRK provide failed: err=%u", err);
		}
		break;
	}
	default:
		LOG_DBG("Unsupported info request: %u", exp_info);
		break;
	}
}

/**
 * @brief LTK request callback from Alif BLE stack
 */
static void on_ltk_req(uint8_t const conidx, uint32_t const metainfo, uint8_t const key_size)
{
	/* Forward to security manager to provide LTK */
	int err = halo_ble_sec_provide_ltk(conidx, key_size);
	if (err) {
		LOG_ERR("LTK provide failed: err=%d", err);
	}
}

/**
 * @brief Numeric compare request callback from Alif BLE stack
 */
static void on_gapc_sec_numeric_compare_req(uint8_t const conidx, uint32_t const metainfo,
					    uint32_t const value)
{
	/* Auto-accept for now (TODO: add user confirmation) */
	gapc_pairing_numeric_compare_rsp(conidx, true);
}

/**
 * @brief Connection parameter update request callback from Alif BLE stack
 */
void on_param_update_req(uint8_t conidx, uint32_t metainfo, const gapc_le_con_param_nego_t *p_param)
{
	gapc_le_update_params_cfm(conidx, true, preferred_connection_param.ce_len_min,
				  preferred_connection_param.ce_len_max);
}

/**
 * @brief Connection parameter updated callback from Alif BLE stack
 */
void on_param_updated(uint8_t conidx, uint32_t metainfo, const gapc_le_con_param_t *p_param)
{
	return;
}

/**
 * @brief Data packet size updated callback from Alif BLE stack
 */
void on_packet_size_updated(uint8_t conidx, uint32_t metainfo, uint16_t max_tx_octets,
			    uint16_t max_tx_time, uint16_t max_rx_octets, uint16_t max_rx_time)
{
	return;
}

/**
 * @brief PHY updated callback from Alif BLE stack
 */
void on_phy_updated(uint8_t conidx, uint32_t metainfo, uint8_t tx_phy, uint8_t rx_phy)
{
	return;
}

/**
 * @brief Subrate updated callback from Alif BLE stack
 */
void on_subrate_updated(uint8_t conidx, uint32_t metainfo,
			const gapc_le_subrate_t *p_subrate_params)
{
	return;
}

void on_tx_change_report(uint8_t conidx, uint32_t metainfo, bool local,
			 const gapc_le_tx_power_report_t *p_report)
{
	LOG_INF("TX Power Change Report: conidx=%u, local=%u, tx_power=%d dBm", conidx, local,
		p_report->tx_pwr);
}
void halo_ble_conn_prepare_reboot(void)
{
	/* Clear both noinit guards so the next boot performs a full cold BLE
	 * start: alif_ble_enable() returns 0 (not -EALREADY) and
	 * halo_ble_conn_init() runs gapm_configure() with the new firmware's
	 * callback addresses.
	 */
	conn_ctx.initialized = 0;
	alif_ble_reset_init_state();
}