/* Copyright Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <errno.h>
#include <zephyr/logging/log.h>

#include "alif_ble.h"
#include "gapm.h"
#include "gap_le.h"
#include "gapc_le.h"
#include "co_buf.h"
#include "prf.h"
#include "gatt_db.h"
#include "gatt_srv.h"
#include "ke_mem.h"

#include "alif/bluetooth/bt_srv_hello.h"

LOG_MODULE_REGISTER(bt_srv_hello, CONFIG_BT_HOST_LOG_LEVEL);

#define BLE_MUTEX_TIMEOUT_MS 10000

#define ATT_16_TO_128_ARRAY(uuid)                                                          \
{                                                                                          \
	(uuid) & 0xFF, (uuid >> 8) & 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0        \
}

#define ATT_128_PRIMARY_SERVICE  ATT_16_TO_128_ARRAY(GATT_DECL_PRIMARY_SERVICE)
#define ATT_128_CHARACTERISTIC   ATT_16_TO_128_ARRAY(GATT_DECL_CHARACTERISTIC)
#define ATT_128_CLIENT_CHAR_CFG  ATT_16_TO_128_ARRAY(GATT_DESC_CLIENT_CHAR_CFG)

#ifndef CONFIG_HELLO_STRING_LENGTH
#define CONFIG_HELLO_STRING_LENGTH 10
#endif /* CONFIG_HELLO_STRING_LENGTH */

/* Service environment */
static struct hello_service_env {
	uint16_t start_hdl;
	uint8_t user_lid;
	uint8_t char0_val[250];
	uint8_t char1_val;
	bool ntf_ongoing;
	uint16_t ntf_cfg;
	uint8_t hello_arr[11]; /* "HelloHello" + null terminator */
	uint8_t hello_arr_index;
} srv_env;

/* Service UUID to pass into gatt_db_svc_add */
static const uint8_t hello_service_uuid[] = HELLO_UUID_128_SVC;

/* GATT database for the service */
static const gatt_att_desc_t hello_att_db[HELLO_IDX_NB] = {
	[HELLO_IDX_SERVICE] = {ATT_128_PRIMARY_SERVICE, ATT_UUID(16) | PROP(RD), 0},

	[HELLO_IDX_CHAR0_CHAR] = {ATT_128_CHARACTERISTIC, ATT_UUID(16) | PROP(RD), 0},
	[HELLO_IDX_CHAR0_VAL] = {HELLO_UUID_128_CHAR0, ATT_UUID(128) | PROP(RD) | PROP(N),
				 OPT(NO_OFFSET)},
	[HELLO_IDX_CHAR0_NTF_CFG] = {ATT_128_CLIENT_CHAR_CFG, ATT_UUID(16) | PROP(RD) | PROP(WR),
				     0},
	[HELLO_IDX_CHAR1_CHAR] = {ATT_128_CHARACTERISTIC, ATT_UUID(16) | PROP(RD), 0},
	[HELLO_IDX_CHAR1_VAL] = {HELLO_UUID_128_CHAR1, ATT_UUID(128) | PROP(WR),
				 OPT(NO_OFFSET) | sizeof(uint16_t)},
};

/* Service callbacks */
/**
 * @brief Handle read requests for service attributes
 *
 * This callback is invoked when a client reads a characteristic or descriptor
 * value from the service. It prepares the appropriate response based on the
 * attribute being read.
 *
 * @param conidx Connection index of the requesting client
 * @param user_lid Local user ID assigned during service registration
 * @param token Token to identify the request
 * @param hdl Handle of the attribute being read
 * @param offset Offset within the attribute value (not supported)
 * @param max_length Maximum length of data that can be read
 */
static void on_att_read_get(uint8_t conidx, uint8_t user_lid, uint16_t token, uint16_t hdl,
			    uint16_t offset, uint16_t max_length)
{
	co_buf_t *p_buf = NULL;
	uint16_t status = GAP_ERR_NO_ERROR;
	uint16_t att_val_len = 0;
	void *att_val = NULL;

	do {
		if (offset != 0) {
			/* Long read not supported for any characteristics within this service */
			status = ATT_ERR_INVALID_OFFSET;
			break;
		}

		uint8_t att_idx = hdl - srv_env.start_hdl;

		switch (att_idx) {
		case HELLO_IDX_CHAR0_VAL:
			att_val_len = CONFIG_HELLO_STRING_LENGTH;
			uint8_t loop_count = (CONFIG_HELLO_STRING_LENGTH / 5);

			if (CONFIG_HELLO_STRING_LENGTH % 5) {
				loop_count += 1;
			}
			for (int i = 0; i < loop_count; i++) {
				memcpy(srv_env.char0_val + i * 5,
				       &srv_env.hello_arr[srv_env.hello_arr_index], 5);
			}
			att_val = srv_env.char0_val;
			LOG_DBG("Preparing response for read request");
			break;

		case HELLO_IDX_CHAR0_NTF_CFG:
			att_val_len = sizeof(srv_env.ntf_cfg);
			att_val = &srv_env.ntf_cfg;
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
	/* No mutex needed - callback runs in BLE thread which already holds the mutex */
	gatt_srv_att_read_get_cfm(conidx, user_lid, token, status, att_val_len, p_buf);
	if (p_buf != NULL) {
		co_buf_release(p_buf);
	}
}

/**
 * @brief Handle write requests for service attributes
 *
 * This callback is invoked when a client writes to a characteristic or descriptor
 * value in the service. It processes the write request and updates the appropriate
 * service state.
 *
 * @param conidx Connection index of the requesting client
 * @param user_lid Local user ID assigned during service registration
 * @param token Token to identify the request
 * @param hdl Handle of the attribute being written
 * @param offset Offset within the attribute value (not supported)
 * @param p_data Buffer containing the data to write
 */
static void on_att_val_set(uint8_t conidx, uint8_t user_lid, uint16_t token, uint16_t hdl,
			   uint16_t offset, co_buf_t *p_data)
{
	uint16_t status = GAP_ERR_NO_ERROR;

	do {
		if (offset != 0) {
			/* Long write not supported for any characteristics in this service */
			status = ATT_ERR_INVALID_OFFSET;
			break;
		}

		uint8_t att_idx = hdl - srv_env.start_hdl;

		switch (att_idx) {
		case HELLO_IDX_CHAR1_VAL: {
			if (sizeof(srv_env.char1_val) != co_buf_data_len(p_data)) {
				LOG_ERR("Incorrect buffer size");
				status = ATT_ERR_INVALID_ATTRIBUTE_VAL_LEN;
			} else {
				memcpy(&srv_env.char1_val, co_buf_data(p_data),
				       sizeof(srv_env.char1_val));
				LOG_DBG("led toggle, state %d", srv_env.char1_val);
			}
			break;
		}

		case HELLO_IDX_CHAR0_NTF_CFG: {
			if (sizeof(uint16_t) != co_buf_data_len(p_data)) {
				LOG_ERR("Incorrect buffer size");
				status = ATT_ERR_INVALID_ATTRIBUTE_VAL_LEN;
			} else {
				uint16_t cfg;

				memcpy(&cfg, co_buf_data(p_data), sizeof(uint16_t));
				if (PRF_CLI_START_NTF == cfg || PRF_CLI_STOP_NTFIND == cfg) {
					srv_env.ntf_cfg = cfg;
				} else {
					/* Indications not supported */
					status = ATT_ERR_REQUEST_NOT_SUPPORTED;
				}
			}
			break;
		}

		default:
			status = ATT_ERR_REQUEST_NOT_SUPPORTED;
			break;
		}
	} while (0);

	/* Send the GATT write confirmation */
	/* No mutex needed - callback runs in BLE thread which already holds the mutex */
	gatt_srv_att_val_set_cfm(conidx, user_lid, token, status);
}

/**
 * @brief Handle completion of notification/indication events
 *
 * This callback is invoked when a notification or indication has been sent
 * to a client. It updates the service state to reflect that the notification
 * process is complete.
 *
 * @param conidx Connection index of the client
 * @param user_lid Local user ID assigned during service registration
 * @param metainfo Metadata about the event (used to identify which characteristic)
 * @param status Status of the send operation
 */
static void on_event_sent(uint8_t conidx, uint8_t user_lid, uint16_t metainfo, uint16_t status)
{
	if (metainfo == HELLO_METAINFO_CHAR0_NTF_SEND) {
		srv_env.ntf_ongoing = false;
	}
}

static const gatt_srv_cb_t gatt_cbs = {
	.cb_att_event_get = NULL,
	.cb_att_info_get = NULL,
	.cb_att_read_get = on_att_read_get,
	.cb_att_val_set = on_att_val_set,
	.cb_event_sent = on_event_sent,
};

int bt_srv_hello_init(void)
{
	uint16_t status;

	/* Initialize hello_arr with the string "HelloHello" */
	static const char hello_str[] = "HelloHello";

	memcpy(srv_env.hello_arr, hello_str, sizeof(hello_str));
	srv_env.hello_arr_index = 0;

	/* Register a GATT user */
	int lock_ret = alif_ble_mutex_lock(K_MSEC(BLE_MUTEX_TIMEOUT_MS));

	if (lock_ret) {
		__ASSERT(false, "BLE mutex lock timeout");
		return -ETIMEDOUT;
	}
	status = gatt_user_srv_register(L2CAP_LE_MTU_MIN, 0, &gatt_cbs, &srv_env.user_lid);
	alif_ble_mutex_unlock();
	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Cannot register GATT user service, error code: 0x%02x", status);
		return -EIO;
	}

	/* Add the GATT service */
	lock_ret = alif_ble_mutex_lock(K_MSEC(BLE_MUTEX_TIMEOUT_MS));

	if (lock_ret) {
		__ASSERT(false, "BLE mutex lock timeout");
		/* No sense to try to unregister if unable to acquire mutex - user is on its own */
		return -ETIMEDOUT;
	}
	status = gatt_db_svc_add(srv_env.user_lid, SVC_UUID(128), hello_service_uuid, HELLO_IDX_NB,
				 NULL, hello_att_db, HELLO_IDX_NB, &srv_env.start_hdl);
	if (status != GAP_ERR_NO_ERROR) {
		gatt_user_unregister(srv_env.user_lid);
		alif_ble_mutex_unlock();
		LOG_ERR("Cannot add GATT service to database, error code: 0x%02x", status);
		return -EIO;
	}
	alif_ble_mutex_unlock();

	LOG_DBG("Hello service initialized");

	return 0;
}

int bt_srv_hello_notify(uint8_t conn_idx, const void *data, uint16_t len)
{
	co_buf_t *p_buf;
	uint16_t status;

	if (srv_env.ntf_ongoing) {
		LOG_WRN("Notification already in progress");
		return -EBUSY;
	}

	if (srv_env.ntf_cfg != PRF_CLI_START_NTF) {
		LOG_WRN("Notifications not enabled by client");
		return -EINVAL;
	}

	status = co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, len, GATT_BUFFER_TAIL_LEN);
	if (status != CO_BUF_ERR_NO_ERROR) {
		LOG_ERR("Cannot allocate buffer for notification");
		return -ENOMEM;
	}

	memcpy(co_buf_data(p_buf), data, len);

	int lock_ret = alif_ble_mutex_lock(K_MSEC(BLE_MUTEX_TIMEOUT_MS));

	if (lock_ret) {
		__ASSERT(false, "BLE mutex lock timeout");
		co_buf_release(p_buf);
		return -ETIMEDOUT;
	}
	status = gatt_srv_event_send(conn_idx, srv_env.user_lid, HELLO_METAINFO_CHAR0_NTF_SEND,
				     GATT_NOTIFY, srv_env.start_hdl + HELLO_IDX_CHAR0_VAL, p_buf);
	alif_ble_mutex_unlock();
	co_buf_release(p_buf);

	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Cannot send notification, error code: 0x%02x", status);
		return -EIO;
	}

	srv_env.ntf_ongoing = true;
	return 0;
}
