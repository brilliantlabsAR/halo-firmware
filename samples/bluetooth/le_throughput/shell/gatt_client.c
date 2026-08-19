/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

#include "gatt_client.h"
#include "gatt_cli.h"
#include "co_utils.h"
#include "co_endian.h"
#include "l2cap_coc.h"
#include "prf_types.h"
#include "gapc.h"
#include "common.h"

LOG_MODULE_REGISTER(gatt_client, LOG_LEVEL_ERR);

/* Procedure identifier */
enum gatt_client_proc_id {
	/* Discover service */
	META_DISCOVER_SVC,
	/* Write Info client characteristic configuration */
	META_WRITE_INFO_CCC,
	/* Read Name */
	META_READ_NAME,
	/* Write Name */
	META_WRITE_DATA,
};

/*
 * GLOBAL VARIABLE DEFINITIONS
 ****************************************************************************************
 */

/* Module environment */
static uint8_t gatt_client_user_local_identifier;
static uint16_t complete_status;
static uint16_t service_handle;

K_SEM_DEFINE(gatt_sync_sem, 0, 1);

#define GATT_DISCOVERY_TIMEOUT 50000

/*
 * GATT USER CLIENT HANDLERS
 ****************************************************************************************
 */

/* This function is called when a full service has been found during a discovery procedure.  */
static void on_discovery_full_service_found(uint8_t conidx, uint8_t user_lid, uint16_t metainfo,
					    uint16_t hdl, uint8_t disc_info, uint8_t nb_att,
					    const gatt_svc_att_t *p_atts)
{
	char uuid_tmp[64];

	LOG_DBG("Discovery full serv! meta=%u, handle=%u, info=%u", metainfo, hdl, disc_info);
	for (size_t iter = 0; iter < nb_att; iter++) {
		if (p_atts[iter].att_type == GATT_ATT_CHAR) {
			switch (p_atts[iter].uuid_type) {
			case GATT_UUID_16: {
				uint16_t uuid;

				memcpy(&uuid, p_atts[iter].uuid, sizeof(uuid));
				snprintk(uuid_tmp, sizeof(uuid_tmp), "%x", uuid);
				break;
			}
			case GATT_UUID_32: {
				uint32_t uuid;

				memcpy(&uuid, p_atts[iter].uuid, sizeof(uuid));
				snprintk(uuid_tmp, sizeof(uuid_tmp), "%x", uuid);
				break;
			}
			case GATT_UUID_128: {
				snprintk(uuid_tmp, sizeof(uuid_tmp),
					 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x"
					 "%02x%02x%02x",
					 p_atts[iter].uuid[0], p_atts[iter].uuid[1],
					 p_atts[iter].uuid[2], p_atts[iter].uuid[3],
					 p_atts[iter].uuid[4], p_atts[iter].uuid[5],
					 p_atts[iter].uuid[6], p_atts[iter].uuid[7],
					 p_atts[iter].uuid[8], p_atts[iter].uuid[9],
					 p_atts[iter].uuid[10], p_atts[iter].uuid[11],
					 p_atts[iter].uuid[12], p_atts[iter].uuid[13],
					 p_atts[iter].uuid[14], p_atts[iter].uuid[15]);
				break;
			}
			default: {
				uuid_tmp[0] = 0;
				break;
			}
			}

			service_handle = p_atts[iter].info.charac.val_hdl;

			LOG_DBG(" [%3u] ATT_CHAR handle=%hu, prop=%hhu, uuid type=%hhu, uuid=%s",
				iter, p_atts[iter].info.charac.val_hdl,
				p_atts[iter].info.charac.prop, p_atts[iter].uuid_type, uuid_tmp);
		}
	}
}

void on_discovery_service_found(uint8_t conidx, uint8_t user_lid, uint16_t metainfo,
				uint16_t start_hdl, uint16_t end_hdl, uint8_t uuid_type,
				const uint8_t *p_uuid)
{
	LOG_DBG("Service found: handle=%u...%u, uuid_type=%u", start_hdl, end_hdl, uuid_type);
}

/* This function is called when GATT client user discovery procedure is over. */
static void on_discovery_completed(uint8_t conidx, uint8_t user_lid, uint16_t metainfo,
				   uint16_t status)
{
	LOG_DBG("Discovery ready. meta=%u, status=%u", metainfo, status);
	complete_status = status;

	k_sem_give(&gatt_sync_sem);
}

/* This function is called during a read procedure when attribute value is retrieved form peer */
/* device. */
static void on_read_attribute_value_received(uint8_t conidx, uint8_t user_lid, uint16_t metainfo,
					     uint16_t hdl, uint16_t offset, co_buf_t *p_data)
{
	extern struct tp_data transmit_throughput_results;

	if (p_data->data_len != sizeof(struct tp_data)) {
		LOG_ERR("Received data error");
		return;
	}

	memcpy(&transmit_throughput_results, (p_data->buf + p_data->head_len), p_data->data_len);

	app_transition_to(APP_STATE_DATA_SEND_READY);
}

/* This function is called when GATT client user read procedure is over. */
static void on_read_completed(uint8_t conidx, uint8_t user_lid, uint16_t metainfo, uint16_t status)
{
	LOG_DBG("Read done. status=%u, meta=%u", status, metainfo);
	complete_status = status;

	k_sem_give(&gatt_sync_sem);
}

/* This function is called when GATT client user write procedure is over. */
static void on_write_completed(uint8_t conidx, uint8_t user_lid, uint16_t metainfo, uint16_t status)
{
	switch (metainfo) {
	/* Discovery completed */
	case META_WRITE_INFO_CCC: {
		LOG_DBG("Info CCC written. status=%u", status);
		break;
	}
	/* Data written */
	case META_WRITE_DATA:
		break;

	default:
		break;
	}
	complete_status = status;
	k_sem_give(&gatt_sync_sem);
}

/* This function is called when a notification or an indication is received onto register handle */
/* range (see #gatt_cli_event_register). */
static void on_ntf_or_ind_received(uint8_t conidx, uint8_t user_lid, uint16_t token,
				   uint8_t evt_type, bool complete, uint16_t hdl, co_buf_t *p_data)
{
	extern struct tp_data receive_throughput_results;
	extern struct tp_data tp_stats;

	static bool clock_cycle_init = true;
	static uint32_t clock_cycles_last;
	static uint64_t accumulated_time_ns;

	uint32_t const cycle_now = k_cycle_get_32();

	size_t const data_len = co_buf_data_len(p_data);

	if (clock_cycle_init) {
		clock_cycle_init = false;
		clock_cycles_last = cycle_now;
		memset(&receive_throughput_results, 0, sizeof(receive_throughput_results));
	}

	accumulated_time_ns += k_cyc_to_ns_floor64(cycle_now - clock_cycles_last);
	clock_cycles_last = cycle_now;

	if (evt_type == GATT_INDICATE) {
		if (accumulated_time_ns) {
			receive_throughput_results.write_rate =
				(((uint64_t)receive_throughput_results.write_len << 3) *
				 1000000000) /
				accumulated_time_ns;
		}

		printk("\r\n");
		if (data_len == sizeof(tp_stats)) {
			memcpy(&tp_stats, co_buf_data(p_data), sizeof(tp_stats));
		} else {
			LOG_ERR("Peer result read failed");
		}
		app_transition_to(APP_STATE_DATA_RECEIVE_READY);

		accumulated_time_ns = 0;
		clock_cycle_init = true;
	} else {
		receive_throughput_results.write_count++;
		receive_throughput_results.write_len += data_len;
		if ((receive_throughput_results.write_count % 256) == 0) {
			printk(".");
		}
	}

	gatt_cli_att_event_cfm(conidx, user_lid, token);
}

/*
 * FUNCTIONS DEFINITIONS
 ****************************************************************************************
 */
void cb_inc_svc(uint8_t conidx, uint8_t user_lid, uint16_t metainfo, uint16_t inc_svc_hdl,
		uint16_t start_hdl, uint16_t end_hdl, uint8_t uuid_type, const uint8_t *p_uuid)
{
	LOG_DBG("cb_inc_svc");
}
void cb_char(uint8_t conidx, uint8_t user_lid, uint16_t metainfo, uint16_t char_hdl,
	     uint16_t val_hdl, uint8_t prop, uint8_t uuid_type, const uint8_t *p_uuid)
{
	LOG_DBG("cb_char");
}
void cb_desc(uint8_t conidx, uint8_t user_lid, uint16_t metainfo, uint16_t desc_hdl,
	     uint8_t uuid_type, const uint8_t *p_uuid)
{
	LOG_DBG("cb_desc");
}
/* GATT Client register and initialize */
int gatt_client_register(void)
{
	/* GATT Client callback handler */
	static const gatt_cli_cb_t _callbacks = {
		.cb_discover_cmp = on_discovery_completed,
		.cb_read_cmp = on_read_completed,
		.cb_write_cmp = on_write_completed,
		.cb_att_val_get = NULL,
		.cb_svc = on_discovery_full_service_found,
		.cb_svc_info = on_discovery_service_found,
		.cb_inc_svc = cb_inc_svc,
		.cb_char = cb_char,
		.cb_desc = cb_desc,
		.cb_att_val = on_read_attribute_value_received,
		.cb_att_val_evt = on_ntf_or_ind_received,
		.cb_svc_changed = NULL,
	};

	/* Register GATT Client User */
	uint16_t const status = gatt_user_cli_register(CONFIG_BLE_MTU_SIZE, 0, &_callbacks,
						       &gatt_client_user_local_identifier);
	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("GATT client register failed: %u", status);
		return -1;
	}
	return 0;
}

/* Discover all primary services */
int gatt_client_discover_primary_all(uint8_t const conidx)
{
	/* Check that the indicated connection well exists */
	if (!gapc_is_established(conidx)) {
		LOG_ERR("Connection not up: %u", conidx);
		return -ENOEXEC;
	}

	/* Start service discovery */
	uint16_t const status =
		gatt_cli_discover_svc(conidx, gatt_client_user_local_identifier, META_DISCOVER_SVC,
				      GATT_DISCOVER_SVC_PRIMARY_ALL, true, GATT_MIN_HDL,
				      GATT_MAX_HDL, GATT_UUID_16, NULL);

	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Discover failed! status=%u", status);
		return -1;
	}

	if (k_sem_take(&gatt_sync_sem, K_MSEC(GATT_DISCOVERY_TIMEOUT)) != 0) {
		LOG_ERR("Discovery timeout!");
		return -1;
	}
	if (complete_status != GAP_ERR_NO_ERROR) {
		return -1;
	}

	return 0;
}

/* Discover specific primary service by UUID */
int gatt_client_discover_primary_by_uuid(struct conn_uuid const uuid,
					 uint16_t *const p_handle_found)
{
	service_handle = UINT16_MAX;

	/* Start service discovery */
	uint16_t status =
		gatt_cli_discover_svc(uuid.conidx, gatt_client_user_local_identifier,
				      META_DISCOVER_SVC, GATT_DISCOVER_SVC_PRIMARY_BY_UUID, true,
				      GATT_MIN_HDL, GATT_MAX_HDL, uuid.uuid_type, uuid.p_uuid);

	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Discover failed! status=%u", status);
		return -1;
	}

	if (k_sem_take(&gatt_sync_sem, K_MSEC(GATT_DISCOVERY_TIMEOUT)) != 0) {
		LOG_ERR("Discovery timeout!");
		return -1;
	}
	if (complete_status != GAP_ERR_NO_ERROR) {
		return -1;
	}
	if (UINT16_MAX == service_handle) {
		LOG_ERR("Handle not found for UUID");
		return -1;
	}

	*p_handle_found = service_handle;

	return 0;
}

int gatt_client_read(struct conn_handle const handle, size_t const size)
{
	/* Ask characteristic read */
	uint16_t const status = gatt_cli_read(handle.conidx, gatt_client_user_local_identifier,
					      META_READ_NAME, handle.handle, 0, size);
	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Read failed! status=%u", status);
		return -EFAULT;
	}

	if (k_sem_take(&gatt_sync_sem, K_MSEC(2000)) != 0) {
		LOG_ERR("Read timeout!");
		return -1;
	}
	if (complete_status != GAP_ERR_NO_ERROR) {
		return -1;
	}
	return 0;
}

int gatt_client_read_by_uuid(struct conn_uuid const uuid, size_t const size)
{
	/* Ask characteristic read */
	uint16_t const status = gatt_cli_read_by_uuid(
		uuid.conidx, gatt_client_user_local_identifier, META_READ_NAME, GATT_MIN_HDL,
		GATT_MAX_HDL, uuid.uuid_type, uuid.p_uuid);
	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Read by UUID failed! status=%u", status);
		return -EFAULT;
	}

	if (k_sem_take(&gatt_sync_sem, K_MSEC(1000)) != 0) {
		LOG_ERR("Read timeout!");
		return -1;
	}
	if (complete_status != GAP_ERR_NO_ERROR) {
		return -1;
	}
	return 0;
}

static int _gatt_client_write_data(struct conn_handle const handle, char const *p_data,
				   size_t const size, uint8_t const writetype)
{
	uint16_t status;
	co_buf_t *p_buf = NULL;

	if (!p_data || !size) {
		return -EINVAL;
	}

	/* Allocate TX buffer */
	status = co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, size, GATT_BUFFER_TAIL_LEN);
	if (CO_BUF_ERR_NO_ERROR != status || !p_buf) {
		LOG_ERR("unable to allocate TX buffer! status=%u", status);
		return -ENOMEM;
	}

	/* Copy data */
	memcpy(co_buf_data(p_buf), p_data, size);

	/* Start attribute write */
	status = gatt_cli_write(handle.conidx, gatt_client_user_local_identifier, META_WRITE_DATA,
				writetype, handle.handle, 0, p_buf);
	co_buf_release(p_buf);

	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Write failed! status=%u", status);
		return -EFAULT;
	}

	if (k_sem_take(&gatt_sync_sem, K_MSEC(1000)) != 0) {
		LOG_ERR("Write timeout!");
		return -1;
	}
	if (complete_status != GAP_ERR_NO_ERROR) {
		LOG_ERR("GAP Write fail. status=%u", complete_status);
		return -1;
	}
	return 0;
}

int gatt_client_write_ack(struct conn_handle const handle, char const *p_data, size_t const size)
{
	return _gatt_client_write_data(handle, p_data, size, GATT_WRITE);
}

int gatt_client_write_noack(struct conn_handle const handle, char const *p_data, size_t const size)
{
	return _gatt_client_write_data(handle, p_data, size, GATT_WRITE_NO_RESP);
}

int gatt_client_register_event(struct conn_handle const handle)
{
	return gatt_cli_event_register(handle.conidx, gatt_client_user_local_identifier,
				       handle.handle, handle.handle);
}
