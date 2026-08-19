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
#include <zephyr/random/rand32.h>
#include "prf.h"
#include "mesh_api.h"
#include "mesh_defines.h"

LOG_MODULE_REGISTER(mesh_node, LOG_LEVEL_DBG);
/* Print the function names that are not implemented */
#define EMPTY_FUNC() LOG_DBG("Not Implemented")

/* Array providing string description of storage update types */
static const char *storage_update_name[M_STORE_UPD_TYPE_MAX] = {
	[M_STORE_UPD_TYPE_NET_KEY_UPDATED] = "Network key updated",
	[M_STORE_UPD_TYPE_NET_KEY_DELETED] = "Network key deleted",
	[M_STORE_UPD_TYPE_APP_KEY_UPDATED] = "Application key updated",
	[M_STORE_UPD_TYPE_APP_KEY_DELETED] = "Application key deleted",
	[M_STORE_UPD_TYPE_PUBLI_PARAM] = "Model publication parameters updated",
	[M_STORE_UPD_TYPE_SUBS_LIST] = "Model subscription list updated",
	[M_STORE_UPD_TYPE_BINDING] = "Model/application key binding updated",
	[M_STORE_UPD_TYPE_STATE] = "State updated",
	[M_STORE_UPD_TYPE_LPN_UPDATED] = "Friendship with LPN updated",
	[M_STORE_UPD_TYPE_LPN_LOST] = "Friendship with LPN lost",
	[M_STORE_UPD_TYPE_FRIEND_UPDATED] = "Friendship with Friend updated",
	[M_STORE_UPD_TYPE_FRIEND_LOST] = "Friendship with Friend lost",
	[M_STORE_UPD_TYPE_IV_SEQ] = "SEQ/IV value updated",
};

/* State of the provisioning
 * Note these defines are in m_defines.h which are not part of
 * API headers. Hence redefined here but in the future they need
 * to be places in API header from stack provider
 */
enum mesh_prov_state {
	/* Provisioning started - procedure started by a provisioner */
	MESH_PROV_STARTED,
	/* Provisioning succeed */
	MESH_PROV_SUCCEED,
	/* Provisioning failed */
	MESH_PROV_FAILED,
};

static void mesh_node_cb_enabled(uint16_t status, bool prov)
{
	if (status == MESH_ERR_NO_ERROR) {
		LOG_DBG("Enabled - %sProvisioned", prov ? "" : "Not ");
	}
}

static void mesh_node_cb_disabled(uint16_t status)
{
	EMPTY_FUNC();
}

static void mesh_node_cb_prov_state(uint8_t state, uint16_t status)
{
	switch (state) {
	case MESH_PROV_STARTED: {
		LOG_DBG("Provisioning started");
	} break;

	case MESH_PROV_SUCCEED: {
		LOG_DBG("Provisioning successful");
	} break;

	case MESH_PROV_FAILED:
	default: {
		LOG_DBG("Provisioning failed (status = 0x%02X)", status);
	} break;
	}
}

static void mesh_node_cb_prov_param_req(void)
{
	m_prov_param_t prov_param;
	uint8_t dev_uuid[MESH_DEV_UUID_LEN];

	sys_rand_get(dev_uuid, sizeof(dev_uuid));

	LOG_DBG("Provisioning parameters requested");

	/* Set the parameters */
	memcpy(&(prov_param.dev_uuid[0]), dev_uuid, MESH_DEV_UUID_LEN - 1);
	prov_param.uri_hash = 0;
	prov_param.oob_info = 0;
	prov_param.pub_key_oob = 0;
	prov_param.out_oob_size = 0;
	prov_param.in_oob_size = 0;
	prov_param.out_oob_action = 0;
	prov_param.in_oob_action = 0;
	prov_param.info = 0;

	m_api_prov_param_rsp(&prov_param);
}

static void mesh_node_cb_prov_auth_req(uint8_t auth_method, uint16_t auth_action, uint8_t auth_size)
{
	EMPTY_FUNC();
}

static void mesh_node_cb_loaded(uint16_t status)
{
	EMPTY_FUNC();
}

static void mesh_node_cb_storage_update(uint8_t upd_type, uint16_t length, uint8_t *p_data)
{
	LOG_DBG("Storage update: %s (length = %d)", storage_update_name[upd_type], length);

	m_api_storage_load(length, p_data);
}

static void mesh_node_cb_attention(uint8_t attention_state)
{
	LOG_DBG("Attention duration = %ds", attention_state);
}

static void mesh_node_cb_compo_data(uint8_t page)
{
	EMPTY_FUNC();
}

static void mesh_node_cb_node_reset(void)
{
	EMPTY_FUNC();
}

static void mesh_node_cb_group_update(bool added, uint16_t appkey_id, m_lid_t app_key_lid)
{
	LOG_DBG("Application key %s (ID = 0x%04X)", added ? "added" : "removed", appkey_id);
}

static void mesh_node_cb_lpn_status(uint16_t status, uint16_t friend_addr)
{
	EMPTY_FUNC();
}

static void mesh_node_cb_lpn_offer(uint16_t friend_addr, uint8_t rx_window, uint8_t queue_size,
				   uint8_t subs_list_size, int8_t rssi)
{
	EMPTY_FUNC();
}

static void mesh_node_cb_proxy_adv_update(uint8_t state, uint8_t reason)
{
	EMPTY_FUNC();
}

static void mesh_node_cb_proxy_filt_status(uint8_t filt_type, uint16_t list_size)
{
	EMPTY_FUNC();
}

static void mesh_node_cb_provee_pub_key_oob(uint8_t *p_pub_key_x, uint8_t *p_pub_key_y)
{
	EMPTY_FUNC();
}

static void mesh_node_cb_prover_scan_node_found(uint32_t uri_hash, uint16_t oob_info, int8_t rssi,
						uint8_t *p_dev_uuid)
{
	EMPTY_FUNC();
}

static void mesh_node_cb_prover_scan_node_found_gatt(uint16_t oob_info, int8_t rssi,
						     uint8_t *p_dev_uuid, uint8_t *p_addr,
						     uint8_t addr_type)
{
	EMPTY_FUNC();
}

static void mesh_node_cb_prover_scan_stopped(uint8_t reason)
{
	EMPTY_FUNC();
}
static void mesh_node_cb_prover_state(uint8_t state, uint16_t status, uint16_t unicast_addr)
{
	EMPTY_FUNC();
}

static void mesh_node_cb_basic_req(uint32_t req_ind_code)
{
	EMPTY_FUNC();
}

static void mesh_node_cb_prover_identify(uint8_t nb_elt, uint16_t algorithms, uint8_t pub_key_type,
					 uint8_t static_oob_type, uint8_t out_oob_size,
					 uint16_t out_oob_action, uint8_t in_oob_size,
					 uint16_t in_oob_action)
{
	EMPTY_FUNC();
}

static void mesh_node_cb_key_gen(uint16_t status, m_lid_t key_lid, uint32_t cmd_code)
{
	EMPTY_FUNC();
}

/* Set of callback functions for communication with Mesh Profile */
static const m_api_cb_t cbs_m_api = {
	.cb_enabled = mesh_node_cb_enabled,
	.cb_disabled = mesh_node_cb_disabled,
	.cb_prov_state = mesh_node_cb_prov_state,
	.cb_prov_param_req = mesh_node_cb_prov_param_req,
	.cb_prov_auth_req = mesh_node_cb_prov_auth_req,
	.cb_loaded = mesh_node_cb_loaded,
	.cb_storage_update = mesh_node_cb_storage_update,
	.cb_attention = mesh_node_cb_attention,
	.cb_compo_data = mesh_node_cb_compo_data,
	.cb_node_reset = mesh_node_cb_node_reset,
	.cb_group_update = mesh_node_cb_group_update,
	.cb_lpn_status = mesh_node_cb_lpn_status,
	.cb_lpn_offer = mesh_node_cb_lpn_offer,
	.cb_proxy_adv_update = mesh_node_cb_proxy_adv_update,
	.cb_proxy_filt_status = mesh_node_cb_proxy_filt_status,
	.cb_provee_pub_key_oob = mesh_node_cb_provee_pub_key_oob,
	.cb_prover_scan_node_found = mesh_node_cb_prover_scan_node_found,
	.cb_prover_scan_node_found_gatt = mesh_node_cb_prover_scan_node_found_gatt,
	.cb_prover_scan_stopped = mesh_node_cb_prover_scan_stopped,
	.cb_prover_state = mesh_node_cb_prover_state,
	.cb_basic_req = mesh_node_cb_basic_req,
	.cb_prover_identify = mesh_node_cb_prover_identify,
	.cb_key_gen = mesh_node_cb_key_gen,
};

static void mesh_node_cb_fault_get(uint16_t comp_id)
{
	EMPTY_FUNC();
}

static void mesh_node_cb_fault_test(uint16_t comp_id, uint8_t test_id, bool cfm_needed)
{
	EMPTY_FUNC();
}

static void mesh_node_cb_fault_clear(uint16_t comp_id)
{
	EMPTY_FUNC();
}

static void mesh_node_cb_fault_period(uint32_t period_ms, uint32_t period_fault_ms)
{
	EMPTY_FUNC();
}

/* Set of callback function for communication with Health Foundation model */
static const m_api_fault_cb_t cbs_m_api_fault = {
	.cb_fault_get = mesh_node_cb_fault_get,
	.cb_fault_test = mesh_node_cb_fault_test,
	.cb_fault_clear = mesh_node_cb_fault_clear,
	.cb_fault_period = mesh_node_cb_fault_period,
};

static void mesh_light_bulb_cb_register_ind(uint32_t model_id, uint8_t elmt_idx, m_lid_t mdl_lid)
{
	EMPTY_FUNC();
}

static void mesh_light_bulb_cb_srv_state_upd_ind(uint16_t state_id, uint8_t elmt_idx,
						 uint32_t state, uint32_t trans_time_ms)
{
	switch (state_id) {
	case MM_STATE_GEN_ONOFF: {
		LOG_INF("Light set to %s", state != 0 ? "ON" : "OFF");
	} break;
	default: {
		LOG_DBG("Other state identifier (%d)", state_id);
	} break;
	}
}

static void mesh_light_bulb_cb_srv_state_req_ind(uint32_t req_ind_code, uint8_t elmt_idx)
{
	EMPTY_FUNC();
}

static void mesh_light_bulb_cb_srv_locg_upd_ind(uint8_t elmt_idx, int32_t latitude,
						int32_t longitude, int16_t altitude)
{
	EMPTY_FUNC();
}

static void mesh_light_bulb_cb_srv_locl_upd_ind(uint8_t elmt_idx, int16_t north, int16_t east,
						int16_t altitude, uint8_t floor,
						uint16_t uncertainty)
{
	EMPTY_FUNC();
}

static void mesh_light_bulb_cb_srv_prop_get_req_ind(uint8_t elmt_idx, uint8_t prop_type,
						    uint16_t prop_id)
{
	EMPTY_FUNC();
}

static void mesh_light_bulb_cb_srv_prop_set_req_ind(uint8_t elmt_idx, uint8_t prop_type,
						    uint16_t prop_id, uint16_t length,
						    uint8_t *p_val)
{
	EMPTY_FUNC();
}

/* Set of callback functions for communication with Mesh Models */
static const mm_api_cb_server_t cbs_mm_api = {
	.cb_srv_state_upd_ind = mesh_light_bulb_cb_srv_state_upd_ind,
	.cb_srv_state_req_ind = mesh_light_bulb_cb_srv_state_req_ind,
	.cb_srv_locg_upd_ind = mesh_light_bulb_cb_srv_locg_upd_ind,
	.cb_srv_locl_upd_ind = mesh_light_bulb_cb_srv_locl_upd_ind,
	.cb_srv_prop_get_req_ind = mesh_light_bulb_cb_srv_prop_get_req_ind,
	.cb_srv_prop_set_req_ind = mesh_light_bulb_cb_srv_prop_set_req_ind,
};

int mesh_node_configure(void)
{
	mesh_cfg_t cfg;
	uint16_t err = 0;

	/* Only PB-ADV supported */
	cfg.prf_cfg.features = 0x0000;
	/* Company identifier assigned by the Bluetooth SIG */
	cfg.prf_cfg.cid = 0x0CDD;
	/* Vendor-assigned product identifier. Set randomnly */
	sys_rand_get(&cfg.prf_cfg.pid, sizeof(cfg.prf_cfg.pid));
	/* Vendor-assigned product version identifier. Set randomnly */
	sys_rand_get(&cfg.prf_cfg.vid, sizeof(cfg.prf_cfg.vid));
	/* Location descriptor. Set randomnly */
	sys_rand_get(&cfg.prf_cfg.loc, sizeof(cfg.prf_cfg.loc));

	/* Other configuration */
	cfg.prf_cfg.nb_addr_replay = 10;
	cfg.prf_cfg.nb_cdata_page = 1;
	cfg.model_cfg.nb_replay = 5;

	err = prf_add_profile(TASK_ID_MESH, 0, 0, &cfg, NULL, NULL);

	if (err) {
		LOG_ERR("Enabling mesh profile failed %u", err);
		return -1;
	}

	err = m_api_set(&cbs_m_api, &cbs_m_api_fault);

	if (err) {
		LOG_ERR("Setting mesh API callbacks failed %u", err);
		return -1;
	}

	err = m_api_enable();
	if (err) {
		LOG_ERR("Mesh API enable failed %u", err);
		return -1;
	}

	err = mm_api_set_server(&cbs_mm_api);
	if (err) {
		LOG_ERR("Setting callback for mesh model failed %u", err);
		return -1;
	}

	err = mm_api_register_server(0, MM_CFG_IDX_GENS_ONOFF, 1, mesh_light_bulb_cb_register_ind);
	if (err) {
		LOG_ERR("Server model registration failed %u", err);
		return -1;
	}

	return 0;
}
