/* Copyright Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <string.h>

#include "gatt_srv.h"
#include "gatt.h"
#include "gatt_msg.h"
#include "gatt_db.h"
#include "ke_mem.h"
#include "prf.h"
#include "mode_switch_service.h"
#include "broadcast_sink.h"
#include "broadcast_source.h"
#include "audio_datapath.h"

LOG_MODULE_REGISTER(mode_switch_service, CONFIG_MODE_SWITCH_SERVICE_LOG_LEVEL);

/* Custom service UUID - Mode Switch Service */
#define MODE_SWITCH_SERVICE_UUID_128 { \
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, \
	0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB \
}

/* Mode toggle characteristic UUID (writeable) */
#define MODE_TOGGLE_CHARACTERISTIC_UUID_128 { \
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, \
	0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFC \
}

/* Mode status characteristic UUID (notify) */
#define MODE_STATUS_CHARACTERISTIC_UUID_128 { \
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, \
	0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFD \
}

/* Service and characteristic handles */

/* Environment for the service */
struct service_env {
	uint16_t start_hdl;
	uint8_t user_lid;
	uint8_t mode_toggle_val;
	uint8_t mode_status_val;
	bool ntf_ongoing;
	uint16_t ntf_cfg;
	mode_switch_type_t active_mode;  /* Current mode */
};

static struct service_env env = {
	.active_mode = MODE_SWITCH_IDLE  /* Initialize to idle mode */
};

/* Forward declaration */
static uint16_t send_mode_notifications(uint32_t conidx_mask);

/* Metainfo constant for notifications */
#define MSS_METAINFO_MODE_STATUS_NTF_SEND 0x1234

/* Mode helper macros */
#define is_mode_active(mode)     (env.active_mode == (mode))
#define set_mode_active(mode)    (env.active_mode = (mode))

/* List of attributes in the service */
enum service_att_list {
	MSS_IDX_SERVICE = 0,
	/* Mode toggle characteristic (writeable) */
	MSS_IDX_MODE_TOGGLE_CHAR,
	MSS_IDX_MODE_TOGGLE_VAL,
	/* Mode status characteristic (notify) */
	MSS_IDX_MODE_STATUS_CHAR,
	MSS_IDX_MODE_STATUS_VAL,
	MSS_IDX_MODE_STATUS_NTF_CFG,
	/* Number of items*/
	MSS_IDX_NB,
};

/* Service UUID to pass into gatt_db_svc_add */
static const uint8_t mss_service_uuid[] = MODE_SWITCH_SERVICE_UUID_128;

/* UUID conversion macros - same pattern as working examples */
#define ATT_16_TO_128_ARRAY(uuid) \
	{(uuid) & 0xFF, (uuid >> 8) & 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}

#define ATT_128_PRIMARY_SERVICE  ATT_16_TO_128_ARRAY(GATT_DECL_PRIMARY_SERVICE)
#define ATT_128_CHARACTERISTIC   ATT_16_TO_128_ARRAY(GATT_DECL_CHARACTERISTIC)
#define ATT_128_CLIENT_CHAR_CFG  ATT_16_TO_128_ARRAY(GATT_DESC_CLIENT_CHAR_CFG)

/* GATT database for the service - using the same pattern as working examples */
static const gatt_att_desc_t mss_att_db[MSS_IDX_NB] = {
	[MSS_IDX_SERVICE] = {ATT_128_PRIMARY_SERVICE, ATT_UUID(16) | PROP(RD), 0},

	[MSS_IDX_MODE_TOGGLE_CHAR] = {ATT_128_CHARACTERISTIC, ATT_UUID(16) | PROP(RD), 0},
	[MSS_IDX_MODE_TOGGLE_VAL] = {
		{0xFC, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
		 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
		ATT_UUID(128) | PROP(WR),
		OPT(NO_OFFSET) | sizeof(uint8_t)
	},

	[MSS_IDX_MODE_STATUS_CHAR] = {ATT_128_CHARACTERISTIC, ATT_UUID(16) | PROP(RD), 0},
	[MSS_IDX_MODE_STATUS_VAL] = {
		{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
		 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFD},
		ATT_UUID(128) | PROP(RD) | PROP(N),
		OPT(NO_OFFSET)
	},
	[MSS_IDX_MODE_STATUS_NTF_CFG] = {ATT_128_CLIENT_CHAR_CFG,
								ATT_UUID(16) | PROP(RD) | PROP(WR),
								0},
};

/* Mode switching functions */
static int switch_to_idle_mode(void)
{
	LOG_INF("Switching to idle mode");

	/* Stop sink if active */
	if (is_mode_active(MODE_SWITCH_SINK)) {
		LOG_INF("Stopping broadcast sink");
		int ret = broadcast_sink_stop();

		if (ret != 0) {
			LOG_ERR("Failed to stop broadcast sink, err %d", ret);
		}
	}

	/* Stop source if active */
	if (is_mode_active(MODE_SWITCH_SOURCE)) {
		LOG_INF("Stopping broadcast source");
		int ret = broadcast_source_stop();

		if (ret != 0) {
			LOG_ERR("Failed to stop broadcast source, err %d", ret);
		}
	}

	/* Set to idle mode */
	set_mode_active(MODE_SWITCH_IDLE);

	/* Send notifications to all connected clients */
	send_mode_notifications(UINT32_MAX);

	return 0;
}

static int switch_to_sink_mode(void)
{
	LOG_INF("Switching to broadcast sink mode");

	/* Check if already in sink mode */
	if (is_mode_active(MODE_SWITCH_SINK)) {
		LOG_INF("Already in sink mode, no action needed");
		return 0;
	}

	/* Stop source if active */
	if (is_mode_active(MODE_SWITCH_SOURCE)) {
		LOG_INF("Stopping broadcast source before switching to sink");
		int ret = broadcast_source_stop();

		if (ret != 0) {
			LOG_ERR("Failed to stop broadcast source, err %d", ret);
		}
	}

	/* Start scanning (modules are configured once during initialization) */
	int ret = broadcast_sink_start_scanning();

	if (ret != 0) {
		LOG_ERR("Failed to start broadcast sink scanning, err %d", ret);
		return ret;
	}

	/* Set sink mode as active */
	set_mode_active(MODE_SWITCH_SINK);

	/* Send notifications to all connected clients */
	send_mode_notifications(UINT32_MAX);

	return 0;
}

static int switch_to_source_mode(void)
{
	LOG_INF("Switching to broadcast source mode");

	/* Check if already in source mode */
	if (is_mode_active(MODE_SWITCH_SOURCE)) {
		LOG_INF("Already in source mode, no action needed");
		return 0;
	}

	/* Stop sink if active */
	if (is_mode_active(MODE_SWITCH_SINK)) {
		LOG_INF("Stopping broadcast sink before switching to source");
		int ret = broadcast_sink_stop();

		if (ret != 0) {
			LOG_ERR("Failed to stop broadcast sink, err %d", ret);
		}
	}

	/* Start broadcasting (modules are configured once during initialization) */
	int ret = broadcast_source_start_broadcasting();

	if (ret != 0) {
		LOG_ERR("Failed to start broadcast source, err %d", ret);
		return ret;
	}

	/* Set source mode as active */
	set_mode_active(MODE_SWITCH_SOURCE);

	/* Send notifications to all connected clients */
	send_mode_notifications(UINT32_MAX);

	return 0;
}

/* Send notifications to all connected clients that have them enabled */
static uint16_t send_mode_notifications(uint32_t conidx_mask)
{
	co_buf_t *p_buf;
	uint16_t status;
	uint8_t conidx = 0; /* For now, just send to connection 0 */

	/* Cannot send another notification unless previous one is completed */
	if (env.ntf_ongoing) {
		return PRF_ERR_REQ_DISALLOWED;
	}

	/* Check notification subscription */
	if (env.ntf_cfg != GATT_CCC_START_NTF) {
		return PRF_ERR_NTF_DISABLED;
	}

	/* Get a buffer to put the notification data into */
	status = co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, sizeof(env.active_mode),
					GATT_BUFFER_TAIL_LEN);
	if (status != CO_BUF_ERR_NO_ERROR) {
		return GAP_ERR_INSUFF_RESOURCES;
	}

	uint8_t mode_val = (uint8_t)env.active_mode;

	memcpy(co_buf_data(p_buf), &mode_val, sizeof(mode_val));
	status = gatt_srv_event_send(conidx, env.user_lid,
					MSS_METAINFO_MODE_STATUS_NTF_SEND, GATT_NOTIFY,
					env.start_hdl + MSS_IDX_MODE_STATUS_VAL, p_buf);

	co_buf_release(p_buf);

	if (status == GAP_ERR_NO_ERROR) {
		env.ntf_ongoing = true;
		LOG_INF("Mode notification sent: mode=%u", env.active_mode);
	}

	return status;
}

/* GATT server callbacks */
static void on_event_sent(uint8_t conidx, uint8_t user_lid, uint16_t metainfo, uint16_t status)
{
	if (metainfo == MSS_METAINFO_MODE_STATUS_NTF_SEND) {
		env.ntf_ongoing = false;
		LOG_DBG("Notification sent, status=%u", status);
	}

	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("GATT event send failed, status %u", status);
	}
}

static void on_att_read_get(uint8_t conidx, uint8_t user_lid, uint16_t token,
	uint16_t hdl, uint16_t offset, uint16_t max_length)
{
	LOG_INF("Read request: conidx=%u, hdl=%u, offset=%u, max_len=%u",
		conidx, hdl, offset, max_length);

	uint8_t mode_val;
	co_buf_t *p_buf = NULL;
	uint16_t status = GAP_ERR_NO_ERROR;
	uint16_t att_val_len = 0;
	void *att_val = NULL;
	uint16_t ccc_value = 0; /* Move declaration here to avoid dangling pointer */

	do {
		if (offset != 0) {
			status = ATT_ERR_INVALID_OFFSET;
			break;
		}

		uint8_t att_idx = hdl - env.start_hdl;

		switch (att_idx) {
		case MSS_IDX_MODE_TOGGLE_VAL:
		case MSS_IDX_MODE_STATUS_VAL:
			mode_val = (uint8_t)env.active_mode;
			att_val_len = sizeof(mode_val);
			att_val = &mode_val;
			break;
		case MSS_IDX_MODE_STATUS_NTF_CFG:
			att_val_len = sizeof(ccc_value);
			att_val = &ccc_value;
			break;
		default:
			break;
		}

		if (att_val == NULL) {
			status = ATT_ERR_REQUEST_NOT_SUPPORTED;
			break;
		}

		status = co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, att_val_len,
						GATT_BUFFER_TAIL_LEN);

		if (status != CO_BUF_ERR_NO_ERROR) {
			status = ATT_ERR_INSUFF_RESOURCE;
			break;
		}

		memcpy(co_buf_data(p_buf), att_val, att_val_len);
	} while (0);

	/* Send the GATT response */
	gatt_srv_att_read_get_cfm(conidx, user_lid, token, status, att_val_len, p_buf);
	if (p_buf != NULL) {
		co_buf_release(p_buf);
	}
}

static void on_att_val_set(uint8_t conidx, uint8_t user_lid, uint16_t token, uint16_t hdl,
	uint16_t offset, co_buf_t *p_data)
{
	uint16_t status = GAP_ERR_NO_ERROR;

	LOG_INF("Write request: conidx=%u, hdl=%u, offset=%u, data_len=%u",
			conidx, hdl, offset, co_buf_data_len(p_data));

	/* Log the data being written for debugging */
	if (co_buf_data_len(p_data) > 0) {
		uint8_t *data = co_buf_data(p_data);

		LOG_INF("Write data: 0x%02X", data[0]);
	}

	do {
		if (offset != 0) {
			status = ATT_ERR_INVALID_OFFSET;
			break;
		}

		uint8_t att_idx = hdl - env.start_hdl;

		LOG_INF("Attribute index: %u (hdl=%u, service_hdl=%u)", att_idx, hdl,
				env.start_hdl);

		switch (att_idx) {
		case MSS_IDX_MODE_TOGGLE_VAL: {
			if (sizeof(uint8_t) != co_buf_data_len(p_data)) {
				LOG_INF("Incorrect buffer size");
				status = ATT_ERR_INVALID_ATTRIBUTE_VAL_LEN;
			} else {
				uint8_t new_mode;

				memcpy(&new_mode, co_buf_data(p_data), sizeof(new_mode));

				LOG_INF("Received mode switch request: %u", new_mode);

				/* Switch to the new mode */
				int ret;

				switch ((mode_switch_type_t) new_mode) {
				case MODE_SWITCH_IDLE:
					ret = switch_to_idle_mode();
					break;
				case MODE_SWITCH_SINK:
					ret = switch_to_sink_mode();
					break;
				case MODE_SWITCH_SOURCE:
					ret = switch_to_source_mode();
					break;
				default:
					__ASSERT(new_mode < MODE_SWITCH_MAX,
						"Invalid mode value: %u", new_mode);
					status = ATT_ERR_REQUEST_NOT_SUPPORTED;
					ret = -1;
					break;
				}

				if (ret == 0) {
					LOG_INF("Successfully switched to mode %u", new_mode);
				} else if (status == GAP_ERR_NO_ERROR) {
					status = ATT_ERR_REQUEST_NOT_SUPPORTED;
				}
			}
			break;
		}

		case MSS_IDX_MODE_STATUS_NTF_CFG: {
			if (sizeof(uint16_t) != co_buf_data_len(p_data)) {
				LOG_INF("Incorrect buffer size");
				status = ATT_ERR_INVALID_ATTRIBUTE_VAL_LEN;
			} else {
				uint16_t cfg;

				memcpy(&cfg, co_buf_data(p_data), sizeof(cfg));

				if (GATT_CCC_START_NTF == cfg || GATT_CCC_STOP_NTFIND == cfg) {
					env.ntf_cfg = cfg;
					if (cfg == GATT_CCC_START_NTF) {
						LOG_INF("Mode status enabled for connection %u",
								conidx);
					} else {
						LOG_INF("Mode status disabled for connection %u",
								conidx);
					}
				} else {
					/* Indications not supported */
					status = ATT_ERR_REQUEST_NOT_SUPPORTED;
				}
			}
			break;
		}

		default:
			LOG_ERR("Unexpected attribute index: %u", att_idx);
			status = ATT_ERR_REQUEST_NOT_SUPPORTED;
			break;
		}
	} while (0);

	LOG_INF("Write response: status=%u", status);
	/* Send the GATT write confirmation */
	gatt_srv_att_val_set_cfm(conidx, user_lid, token, status);
}

static const gatt_srv_cb_t gatt_cbs = {
	.cb_att_event_get = NULL,
	.cb_att_info_get = NULL,
	.cb_att_read_get = on_att_read_get,
	.cb_att_val_set = on_att_val_set,
	.cb_event_sent = on_event_sent,
};

/* Add service to the stack */
static uint16_t service_init(void)
{
	uint16_t status;

	/* Register a GATT user */
	status = gatt_user_srv_register(L2CAP_LE_MTU_MIN, 0, &gatt_cbs, &env.user_lid);
	if (status != GAP_ERR_NO_ERROR) {
		return status;
	}

	/* Add the GATT service */
	status = gatt_db_svc_add(env.user_lid, SVC_UUID(128), mss_service_uuid, MSS_IDX_NB, NULL,
						mss_att_db, MSS_IDX_NB, &env.start_hdl);
	if (status != GAP_ERR_NO_ERROR) {
		gatt_user_unregister(env.user_lid);
		return status;
	}

	LOG_INF("Mode switch service handle: %u", env.start_hdl);

	return GAP_ERR_NO_ERROR;
}

int mode_switch_service_start(void)
{
	LOG_INF("Starting mode switch service");

	/* Initialize the service */
	uint16_t err = service_init();

	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to initialize mode switch service, err %u", err);
		return err;
	}

	LOG_INF("Mode switch service started successfully");
	LOG_INF("Service handle: %u, GATT user LID: %u", env.start_hdl, env.user_lid);
	return 0;
}

int mode_switch_service_stop(void)
{
	if (env.user_lid != 0) {
		gatt_user_unregister(env.user_lid);
		env.user_lid = 0;
	}

	LOG_INF("Mode switch service stopped");
	return 0;
}

mode_switch_type_t mode_switch_service_get_mode(void)
{
	return env.active_mode;
}


int mode_switch_service_set_mode(mode_switch_type_t mode)
{
	/* Switch to the new mode */
	int ret;

	switch (mode) {
	case MODE_SWITCH_IDLE:
		ret = switch_to_idle_mode();
		break;
	case MODE_SWITCH_SINK:
		ret = switch_to_sink_mode();
		break;
	case MODE_SWITCH_SOURCE:
		ret = switch_to_source_mode();
		break;
	default:
		__ASSERT(mode < MODE_SWITCH_MAX, "Invalid mode value: %u", mode);
		return -EINVAL;
	}

	/* Note: notifications are already sent by the mode switching functions */
	return ret;
}
