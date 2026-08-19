/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <mgmt/mcumgr/transport/smp_internal.h>
#include <app_version.h>
#include "alif_ble.h"
#include "gapm.h"
#include "gapm_le_init.h"
#include "gap_le.h"
#include "gapc.h"
#include "gapc_le.h"
#include "gapm_le_adv.h"
#include "gapc_sec.h"
#include "gatt_db.h"
#include "gatt_srv.h"

#include "halo/ble_manager.h"
#include "halo/ble_connection.h"
#include "halo/ble_service.h"
#include "halo/ble_ota.h"

LOG_MODULE_REGISTER(halo_ble_ota, CONFIG_HALO_LOG_LEVEL);

#define BLE_OTA_INIT_MAGIC 0x4F544154 /* 'OTAT' */

/* Helper macros for 128-bit UUID conversion */
#define ATT_16_TO_128_ARRAY(uuid)                                                                  \
	{(uuid) & 0xFF, (uuid >> 8) & 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}

#define ATT_128_PRIMARY_SERVICE ATT_16_TO_128_ARRAY(GATT_DECL_PRIMARY_SERVICE)
#define ATT_128_CHARACTERISTIC  ATT_16_TO_128_ARRAY(GATT_DECL_CHARACTERISTIC)
#define ATT_128_CLIENT_CHAR_CFG ATT_16_TO_128_ARRAY(GATT_DESC_CLIENT_CHAR_CFG)

/* SMP service 8D53DC1D-1DB7-4CD3-868B-8A527460AA84 */
#define OTA_UUID_128_SVC                                                                           \
	{0x84, 0xAA, 0x60, 0x74, 0x52, 0x8A, 0x8B, 0x86,                                           \
	 0xD3, 0x4C, 0xB7, 0x1D, 0x1D, 0xDC, 0x53, 0x8D}

/* SMP characteristic DA2E7828-FBCE-4E01-AE9E-261174997C48 */
#define OTA_UUID_128_CHAR                                                                          \
	{0x48, 0x7C, 0x99, 0x74, 0x11, 0x26, 0x9E, 0xAE,                                           \
	 0x01, 0x4E, 0xCE, 0xFB, 0x28, 0x78, 0x2E, 0xDA}

enum ota_srv_att_list {
	OTA_IDX_SERVICE = 0,
	OTA_IDX_CHAR,
	OTA_IDX_VAL,
	OTA_IDX_NTF_CFG,
	OTA_IDX_NB,
};

struct ota_srv_ctx {
	uint32_t initialized;
	uint16_t start_hdl;
	uint8_t user_lid;
	uint8_t conidx;
	uint16_t ntf_cfg;
	struct k_sem ntf_sem;
	struct smp_transport transport;
} ota_ctx __attribute__((noinit));

static const uint8_t ota_srv_uuid[] = OTA_UUID_128_SVC;

/* The SMP value carries firmware-update / device-management (MCUmgr) traffic,
 * so it requires an encrypted link: SEC_LVL(WP, UNAUTH) gates writes and
 * SEC_LVL(NIP, UNAUTH) gates notifications at the controller, before
 * ota_process_smp_req() ever sees the data. UNAUTH is the ceiling for this
 * Just Works (NO_INPUT_NO_OUTPUT) device - AUTH/SECURE_CON would lock DFU out
 * entirely. Image authenticity is still separately enforced by MCUboot. */
static const gatt_att_desc_t ota_srv_att_db[OTA_IDX_NB] = {
	[OTA_IDX_SERVICE] = {ATT_128_PRIMARY_SERVICE, ATT_UUID(16) | PROP(RD), 0},
	[OTA_IDX_CHAR] = {ATT_128_CHARACTERISTIC, ATT_UUID(16) | PROP(RD), 0},
	[OTA_IDX_VAL] = {OTA_UUID_128_CHAR,
			 ATT_UUID(128) | PROP(WC) | PROP(WR) | PROP(N) | SEC_LVL(WP, UNAUTH) |
				 SEC_LVL(NIP, UNAUTH),
			 OPT(NO_OFFSET) | CFG_MAX_LE_MTU},
	[OTA_IDX_NTF_CFG] = {ATT_128_CLIENT_CHAR_CFG,
			     ATT_UUID(16) | PROP(RD) | PROP(WR) | SEC_LVL(WP, UNAUTH),
			     PRF_CCC_DESC_LEN | OPT(NO_OFFSET)},
};

static uint16_t ota_process_smp_req(const void *p_data, uint16_t len)
{
	struct net_buf *nb;

	nb = smp_packet_alloc();
	if (!nb) {
		return ATT_ERR_INSUFF_RESOURCE;
	}

	if (net_buf_tailroom(nb) < len) {
		smp_packet_free(nb);
		return ATT_ERR_INSUFF_RESOURCE;
	}

	net_buf_add_mem(nb, p_data, len);
	smp_rx_req(&ota_ctx.transport, nb);

	return GAP_ERR_NO_ERROR;
}

static uint16_t ota_process_ntf_cfg_req(const void *p_data, uint16_t len)
{
	uint16_t cfg;

	if (len != PRF_CCC_DESC_LEN) {
		return ATT_ERR_INVALID_ATTRIBUTE_VAL_LEN;
	}

	memcpy(&cfg, p_data, PRF_CCC_DESC_LEN);

	if (cfg != PRF_CLI_START_NTF && cfg != PRF_CLI_STOP_NTFIND) {
		return ATT_ERR_REQUEST_NOT_SUPPORTED;
	}

	ota_ctx.ntf_cfg = cfg;

	return GAP_ERR_NO_ERROR;
}

static uint16_t ota_send_ntf(const void *p_data, uint16_t len)
{
	uint16_t rc;
	co_buf_t *p_buf;

	k_sem_reset(&ota_ctx.ntf_sem);
	uint8_t conidx = halo_ble_get_conidx();

	alif_ble_mutex_lock(K_FOREVER);

	do {
		rc = co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, len, GATT_BUFFER_TAIL_LEN);
		if (rc != CO_BUF_ERR_NO_ERROR) {
			rc = GAP_ERR_INSUFF_RESOURCES;
			break;
		}

		memcpy(co_buf_data(p_buf), p_data, len);
		rc = gatt_srv_event_send(conidx, ota_ctx.user_lid, 0, GATT_NOTIFY,
					 ota_ctx.start_hdl + OTA_IDX_VAL, p_buf);
		co_buf_release(p_buf);
	} while (false);

	alif_ble_mutex_unlock();

	if (rc == GAP_ERR_NO_ERROR) {
		k_sem_take(&ota_ctx.ntf_sem, K_FOREVER);
	}

	return rc;
}

static int transport_out(struct net_buf *nb)
{
	int rc = MGMT_ERR_EOK;
	uint16_t off = 0;
	uint16_t mtu;
	uint16_t tx_size;
	uint16_t tx_rc;

	/* SMP response packet might be bigger than MTU, transmit response in MTU size chunks */

	mtu = halo_ble_get_mtu();

	while (off < nb->len) {
		tx_size = MIN(nb->len - off, mtu);

		tx_rc = ota_send_ntf(&nb->data[off], tx_size);
		if (tx_rc != GAP_ERR_NO_ERROR) {
			LOG_ERR("Failed to send notification, error: %u", tx_rc);
			rc = MGMT_ERR_EUNKNOWN;
			break;
		}

		off += tx_size;
	}

	smp_packet_free(nb);

	return rc;
}

static uint16_t transport_get_mtu(const struct net_buf *nb)
{
	ARG_UNUSED(nb);
	/* Seems that current SMP implementation does not call get_mtu at all, so output function
	 * needs to handle MTU by itself.
	 */
	return halo_ble_get_mtu();
}

static bool transport_query_valid_check(struct net_buf *nb, void *arg)
{
	ARG_UNUSED(nb);
	ARG_UNUSED(arg);
	/* Mark all pending requests invalid when smp_rx_remove_invalid is called on disconnection.
	 */
	return false;
}

/* BLE service callbacks */
static int ble_ota_on_connected(uint8_t conidx)
{
	ARG_UNUSED(conidx);
	return 0;
}

static int ble_ota_on_disconnected(uint8_t conidx)
{
	ARG_UNUSED(conidx);
	smp_rx_remove_invalid(&ota_ctx.transport, NULL);
	k_sem_give(&ota_ctx.ntf_sem);
	return 0;
}

static void on_cb_event_sent(uint8_t conidx, uint8_t user_lid, uint16_t metainfo, uint16_t status)
{
	halo_ble_conn_update_activity();
	k_sem_give(&ota_ctx.ntf_sem);
}

static void on_cb_att_read_get(uint8_t conidx, uint8_t user_lid, uint16_t token, uint16_t hdl,
			       uint16_t offset, uint16_t max_length)
{
	co_buf_t *p_buf = NULL;
	uint16_t status = GAP_ERR_NO_ERROR;
	uint16_t att_val_len = 0;
	void *att_val = NULL;

	uint8_t att_idx = hdl - ota_ctx.start_hdl;

	halo_ble_conn_update_activity();

	switch (att_idx) {
	case OTA_IDX_NTF_CFG:
		att_val_len = sizeof(ota_ctx.ntf_cfg);
		att_val = &ota_ctx.ntf_cfg;
		break;
	default:
		status = ATT_ERR_REQUEST_NOT_SUPPORTED;
		break;
	}

	if (status == GAP_ERR_NO_ERROR) {
		co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, att_val_len, GATT_BUFFER_TAIL_LEN);
		if (p_buf != NULL) {
			memcpy(co_buf_data(p_buf), att_val, att_val_len);
		}
	}

	gatt_srv_att_read_get_cfm(conidx, user_lid, token, status, att_val_len, p_buf);
	if (p_buf != NULL) {
		co_buf_release(p_buf);
	}
}

static void on_cb_att_val_set(uint8_t conidx, uint8_t user_lid, uint16_t token, uint16_t hdl,
			      uint16_t offset, co_buf_t *p_data)
{
	uint16_t rc;
	uint16_t idx = hdl - ota_ctx.start_hdl;

	halo_ble_conn_update_activity();

	switch (idx) {
	case OTA_IDX_VAL:
		rc = ota_process_smp_req(co_buf_data(p_data), co_buf_data_len(p_data));
		if (rc != GAP_ERR_NO_ERROR) {
			LOG_ERR("Failed to process SMP request (conidx: %u), error: %u", conidx,
				rc);
			break;
		}
		break;

	case OTA_IDX_NTF_CFG:
		rc = ota_process_ntf_cfg_req(co_buf_data(p_data), co_buf_data_len(p_data));
		if (rc != GAP_ERR_NO_ERROR) {
			LOG_ERR("Failed to process notification configuration (conidx: %u), error: "
				"%u",
				conidx, rc);
			break;
		}
		break;

	default:
		LOG_ERR("Value set to unknown characteristic (conidx: %u), idx: %u", conidx, idx);
		rc = ATT_ERR_REQUEST_NOT_SUPPORTED;
		break;
	}

	rc = gatt_srv_att_val_set_cfm(conidx, user_lid, token, rc);
	if (rc != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to confirm value set (conidx: %u), error: %u", conidx, rc);
	}
}

static const gatt_srv_cb_t gatt_cbs = {
	.cb_att_event_get = NULL,
	.cb_att_info_get = NULL,
	.cb_event_sent = on_cb_event_sent,
	.cb_att_read_get = on_cb_att_read_get,
	.cb_att_val_set = on_cb_att_val_set,
};

/* BLE service structure */
static struct halo_ble_service ota_service = {
	.name = "OTA Service",
	.init = NULL, /* Initialized separately */
	.deinit = NULL,
	.on_connected = ble_ota_on_connected,
	.on_disconnected = ble_ota_on_disconnected,
	.on_paired = NULL,
};

/* Surface OTA/DFU lifecycle transitions in the firmware log. These fire from the MCUmgr
 * img_mgmt group (needs CONFIG_MCUMGR_MGMT_NOTIFICATION_HOOKS + CONFIG_MCUMGR_GRP_IMG_STATUS_HOOKS).
 * Guarded because this source is also compiled into the MCUboot image, which enables neither.
 */
#if defined(CONFIG_MCUMGR_GRP_IMG_STATUS_HOOKS)
static enum mgmt_cb_return ota_dfu_cb(uint32_t event, enum mgmt_cb_return prev_status, int32_t *rc,
				      uint16_t *group, bool *abort_more, void *data, size_t data_size)
{
	ARG_UNUSED(prev_status);
	ARG_UNUSED(rc);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	ARG_UNUSED(data);
	ARG_UNUSED(data_size);

	switch (event) {
	case MGMT_EVT_OP_IMG_MGMT_DFU_STARTED:
		LOG_DBG("OTA update started");
		break;
	case MGMT_EVT_OP_IMG_MGMT_DFU_PENDING:
		LOG_DBG("OTA update transfer complete, image pending confirmation");
		break;
	case MGMT_EVT_OP_IMG_MGMT_DFU_CONFIRMED:
		LOG_DBG("OTA update image confirmed");
		break;
	case MGMT_EVT_OP_IMG_MGMT_DFU_STOPPED:
		LOG_ERR("OTA update stopped (failed or aborted)");
		break;
	default:
		break;
	}

	return MGMT_CB_OK;
}

static struct mgmt_callback ota_dfu_callback = {
	.callback = ota_dfu_cb,
	.event_id = MGMT_EVT_OP_IMG_MGMT_DFU_STARTED | MGMT_EVT_OP_IMG_MGMT_DFU_PENDING |
		    MGMT_EVT_OP_IMG_MGMT_DFU_CONFIRMED | MGMT_EVT_OP_IMG_MGMT_DFU_STOPPED,
};
#endif /* CONFIG_MCUMGR_GRP_IMG_STATUS_HOOKS */

int halo_ble_ota_init(bool reset)
{
	if (ota_ctx.initialized == BLE_OTA_INIT_MAGIC) {
		return 0;
	}

	k_sem_init(&ota_ctx.ntf_sem, 1, 1);

	int err = 0;
	memset(&ota_ctx.transport, 0, sizeof(ota_ctx.transport));
	ota_ctx.transport.functions.output = transport_out;
	ota_ctx.transport.functions.get_mtu = transport_get_mtu;
	ota_ctx.transport.functions.query_valid_check = transport_query_valid_check;

	if (smp_transport_init(&ota_ctx.transport) != 0) {
		LOG_ERR("Failed to initialize SMP transport");
		return -1;
	}

#if defined(CONFIG_MCUMGR_GRP_IMG_STATUS_HOOKS)
	mgmt_callback_register(&ota_dfu_callback);
#endif

	if (reset) {
		LOG_DBG("Registering OTA service");
		err = gatt_user_srv_register(CFG_MAX_LE_MTU, 0, &gatt_cbs, &ota_ctx.user_lid);
		if (err != GAP_ERR_NO_ERROR) {
			LOG_ERR("gatt_user_srv_register failed");
			return err;
		}
		ota_ctx.start_hdl = GATT_INVALID_HDL;
		ota_ctx.ntf_cfg = 0;
		ota_ctx.conidx = GAP_INVALID_CONIDX;
		err = gatt_db_svc_add(ota_ctx.user_lid, SVC_UUID(128), ota_srv_uuid, OTA_IDX_NB,
				      NULL, ota_srv_att_db, OTA_IDX_NB, &ota_ctx.start_hdl);
		if (err != GAP_ERR_NO_ERROR) {
			LOG_ERR("gatt_db_svc_add failed");
			return -EIO;
		}
	}
	/* Register BLE service */
	err = halo_ble_service_register(&ota_service);
	if (err < 0) {
		LOG_ERR("Failed to register BLE service: %d", err);
		return err;
	}

	ota_ctx.initialized = BLE_OTA_INIT_MAGIC;

	return 0;
}

int halo_ble_ota_deinit(void)
{
	if (ota_ctx.initialized != BLE_OTA_INIT_MAGIC) {
		LOG_WRN("BLE OTA Service not initialized");
		return 0;
	}

	ota_ctx.initialized = 0;

	LOG_DBG("BLE OTA Service deinitialized");

	return 0;
}