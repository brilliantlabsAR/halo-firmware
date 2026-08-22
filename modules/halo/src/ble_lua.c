/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>
#include <string.h>

#include "alif_ble.h"
#include "gatt_db.h"
#include "gatt_srv.h"
#include "gap_le.h"
#include "co_endian.h"

#include "halo/ble_lua.h"
#include "halo/ble_manager.h"
#include "halo/ble_service.h"
#include "halo/mem_manager.h"
#include "halo/pm_manager.h"

LOG_MODULE_REGISTER(halo_ble_lua, CONFIG_HALO_LOG_LEVEL);

/* Default buffer sizes if not configured */
#ifndef CONFIG_HALO_LUA_MAX_REPL_SIZE
#define CONFIG_HALO_LUA_MAX_REPL_SIZE 1024
#endif

#ifndef CONFIG_HALO_LUA_MAX_DATA_SIZE
#define CONFIG_HALO_LUA_MAX_DATA_SIZE 4096
#endif

#ifndef CONFIG_HALO_AUDIO_MAX_DATA_SIZE
#define CONFIG_HALO_AUDIO_MAX_DATA_SIZE 8192
#endif

/* Metainfo for different notification types */
#define LUA_METAINFO_TX_SEND    0x1234
#define LUA_METAINFO_VIDEO_SEND 0x5678
#define LUA_METAINFO_AUDIO_SEND 0x9ABC

#define BLE_LUA_INIT_MAGIC 0x4C554154 /* 'LUAT' */

/* GATT Attribute indices */
enum lua_att_idx {
	LUA_IDX_SERVICE = 0,
	LUA_IDX_RX_CHAR,
	LUA_IDX_RX_VAL,
	LUA_IDX_TX_CHAR,
	LUA_IDX_TX_VAL,
	LUA_IDX_TX_CCC,
	LUA_IDX_VIDEO_CHAR,
	LUA_IDX_VIDEO_VAL,
	LUA_IDX_VIDEO_CCC,
	LUA_IDX_AUDIO_RX_CHAR,
	LUA_IDX_AUDIO_RX_VAL,
	LUA_IDX_AUDIO_TX_CHAR,
	LUA_IDX_AUDIO_TX_VAL,
	LUA_IDX_AUDIO_TX_CCC,
	LUA_IDX_NB,
};

/* BLE Lua Service context */
struct ble_lua_ctx {
	uint32_t initialized;
	uint16_t start_hdl;
	uint8_t user_lid;
	uint8_t conidx;

	/* CCC configurations */
	uint16_t tx_ccc_cfg;
	uint16_t video_ccc_cfg;
	uint16_t audio_tx_ccc_cfg;

	/* REPL ring buffer */
	uint8_t *repl_rx_buf;
	struct ring_buf repl_rx_ring;
	struct k_sem repl_rx_sem;

	/* Data ring buffer */
	uint8_t *data_rx_buf;
	struct ring_buf data_rx_ring;
	struct k_sem data_rx_sem;
	struct ring_buf *current_rx_ring; /* Points to current active ring */

	/* Audio ring buffer */
	uint8_t *audio_rx_buf;
	struct ring_buf audio_rx_ring;
	struct k_sem audio_rx_sem;

	/* Notification flow control */
	struct k_sem tx_ntf_sem;
	struct k_sem video_ntf_sem;
	struct k_sem audio_ntf_sem;

	/* Mutex for write operations */
	struct k_mutex write_lock;

	/* Control handler callback */
	halo_ble_lua_ctrl_handler_t ctrl_handler;
} lua_ctx __attribute__((noinit));

/* Helper macros for 128-bit UUID conversion */
#define ATT_128_TO_ARRAY(uuid)                                                                     \
	{uuid[0], uuid[1], uuid[2],  uuid[3],  uuid[4],  uuid[5],  uuid[6],  uuid[7],              \
	 uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]}

#define ATT_128_PRIMARY_SERVICE ATT_16_TO_128_ARRAY(GATT_DECL_PRIMARY_SERVICE)
#define ATT_128_CHARACTERISTIC  ATT_16_TO_128_ARRAY(GATT_DECL_CHARACTERISTIC)
#define ATT_128_CLIENT_CHAR_CFG ATT_16_TO_128_ARRAY(GATT_DESC_CLIENT_CHAR_CFG)

#define ATT_16_TO_128_ARRAY(uuid)                                                                  \
	{(uuid) & 0xFF, (uuid >> 8) & 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}

/* Service UUID */
static const uint8_t lua_srv_uuid[] = HALO_LUA_UUID_128_SVC;

/* GATT attribute database.
 *
 * Writable value/CCC attributes carry SEC_LVL(WP, UNAUTH) and notify values
 * SEC_LVL(NIP, UNAUTH) so the controller enforces an encrypted link before any
 * write reaches on_att_val_set() (or any notification leaves the device),
 * independent of the software halo_ble_is_paired() gate. UNAUTH (Security Mode
 * 1 Level 2 - encrypted, unauthenticated) is the ceiling this device can reach:
 * pairing is Just Works (GAP_IO_CAP_NO_INPUT_NO_OUTPUT), so AUTH/SECURE_CON
 * would make these attributes permanently inaccessible. Do not "upgrade" it. */
static const gatt_att_desc_t lua_att_db[LUA_IDX_NB] = {
	/* Service Declaration */
	[LUA_IDX_SERVICE] = {ATT_128_PRIMARY_SERVICE, ATT_UUID(16) | PROP(RD), 0},

	/* RX Characteristic */
	[LUA_IDX_RX_CHAR] = {ATT_128_CHARACTERISTIC, ATT_UUID(16) | PROP(RD), 0},
	[LUA_IDX_RX_VAL] = {HALO_LUA_UUID_128_RX,
			    ATT_UUID(128) | PROP(WC) | PROP(WR) | SEC_LVL(WP, UNAUTH),
			    CFG_ATT_VAL_MAX},

	/* TX Characteristic */
	[LUA_IDX_TX_CHAR] = {ATT_128_CHARACTERISTIC, ATT_UUID(16) | PROP(RD), 0},
	[LUA_IDX_TX_VAL] = {HALO_LUA_UUID_128_TX, ATT_UUID(128) | PROP(N) | SEC_LVL(NIP, UNAUTH),
			    OPT(NO_OFFSET)},
	[LUA_IDX_TX_CCC] = {ATT_128_CLIENT_CHAR_CFG,
			    ATT_UUID(16) | PROP(RD) | PROP(WC) | PROP(WR) | SEC_LVL(WP, UNAUTH),
			    OPT(NO_OFFSET) | 2},

	/* Video Characteristic */
	[LUA_IDX_VIDEO_CHAR] = {ATT_128_CHARACTERISTIC, ATT_UUID(16) | PROP(RD), 0},
	[LUA_IDX_VIDEO_VAL] = {HALO_LUA_UUID_128_VIDEO,
			       ATT_UUID(128) | PROP(N) | SEC_LVL(NIP, UNAUTH), OPT(NO_OFFSET)},
	[LUA_IDX_VIDEO_CCC] = {ATT_128_CLIENT_CHAR_CFG,
			       ATT_UUID(16) | PROP(RD) | PROP(WC) | PROP(WR) | SEC_LVL(WP, UNAUTH),
			       OPT(NO_OFFSET) | 2},

	/* Audio RX Characteristic */
	[LUA_IDX_AUDIO_RX_CHAR] = {ATT_128_CHARACTERISTIC, ATT_UUID(16) | PROP(RD), 0},
	[LUA_IDX_AUDIO_RX_VAL] = {HALO_LUA_UUID_128_AUDIO_RX,
				  ATT_UUID(128) | PROP(WC) | PROP(WR) | SEC_LVL(WP, UNAUTH),
				  CFG_ATT_VAL_MAX},

	/* Audio TX Characteristic */
	[LUA_IDX_AUDIO_TX_CHAR] = {ATT_128_CHARACTERISTIC, ATT_UUID(16) | PROP(RD), 0},
	[LUA_IDX_AUDIO_TX_VAL] = {HALO_LUA_UUID_128_AUDIO_TX,
				  ATT_UUID(128) | PROP(N) | SEC_LVL(NIP, UNAUTH), OPT(NO_OFFSET)},
	[LUA_IDX_AUDIO_TX_CCC] = {ATT_128_CLIENT_CHAR_CFG,
				  ATT_UUID(16) | PROP(RD) | PROP(WC) | PROP(WR) | SEC_LVL(WP, UNAUTH),
				  OPT(NO_OFFSET) | 2},
};

/* Forward declarations */
static void on_att_read_get(uint8_t conidx, uint8_t user_lid, uint16_t token, uint16_t hdl,
			    uint16_t offset, uint16_t max_length);
static void on_att_val_set(uint8_t conidx, uint8_t user_lid, uint16_t token, uint16_t hdl,
			   uint16_t offset, co_buf_t *p_data);
static void on_event_sent(uint8_t conidx, uint8_t user_lid, uint16_t metainfo, uint16_t status);
static int ble_lua_on_connected(uint8_t conidx);
static int ble_lua_on_disconnected(uint8_t conidx);

/* GATT read callback */
static void on_att_read_get(uint8_t conidx, uint8_t user_lid, uint16_t token, uint16_t hdl,
			    uint16_t offset, uint16_t max_length)
{
	co_buf_t *p_buf = NULL;
	uint16_t status = GAP_ERR_NO_ERROR;
	uint16_t att_val_len = 0;
	uint8_t att_idx = hdl - lua_ctx.start_hdl;

	switch (att_idx) {
	case LUA_IDX_TX_CCC:
		att_val_len = 2;
		status = co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, att_val_len,
				      GATT_BUFFER_TAIL_LEN);
		if (status == CO_BUF_ERR_NO_ERROR && p_buf != NULL) {
			co_write16(co_buf_data(p_buf), co_htole16(lua_ctx.tx_ccc_cfg));
		}
		break;

	case LUA_IDX_VIDEO_CCC:
		att_val_len = 2;
		status = co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, att_val_len,
				      GATT_BUFFER_TAIL_LEN);
		if (status == CO_BUF_ERR_NO_ERROR && p_buf != NULL) {
			co_write16(co_buf_data(p_buf), co_htole16(lua_ctx.video_ccc_cfg));
		}
		break;

	case LUA_IDX_AUDIO_TX_CCC:
		att_val_len = 2;
		status = co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, att_val_len,
				      GATT_BUFFER_TAIL_LEN);
		if (status == CO_BUF_ERR_NO_ERROR && p_buf != NULL) {
			co_write16(co_buf_data(p_buf), co_htole16(lua_ctx.audio_tx_ccc_cfg));
		}
		break;

	default:
		status = ATT_ERR_REQUEST_NOT_SUPPORTED;
		break;
	}

	gatt_srv_att_read_get_cfm(conidx, user_lid, token, status, att_val_len, p_buf);

	if (p_buf != NULL) {
		co_buf_release(p_buf);
	}
}

/* GATT write callback */
static void on_att_val_set(uint8_t conidx, uint8_t user_lid, uint16_t token, uint16_t hdl,
			   uint16_t offset, co_buf_t *p_data)
{
	uint16_t status = GAP_ERR_NO_ERROR;
	uint8_t att_idx = hdl - lua_ctx.start_hdl;
	uint16_t len = co_buf_data_len(p_data);
	uint8_t *data = co_buf_data(p_data);

	if (halo_pm_is_sleeping()) {
		halo_pm_wakeup(HALO_PM_WAKEUP_BLE);
	}

	switch (att_idx) {
	case LUA_IDX_RX_VAL: {
		struct ring_buf *target_ring;
		struct k_sem *target_sem;

		if (len == 0) {
			break;
		}

		if (halo_ble_is_paired() == false) {
			LOG_WRN("Write attempt while not paired - rejected");
			status = ATT_ERR_INSUFF_AUTHEN;
			break;
		}

		/* Handle control commands and data routing */
		if (offset == 0 && len > 0) {
			/* Check for control commands */
			if (data[0] == HALO_LUA_CTRL_REBOOT ||
			    data[0] == HALO_LUA_CTRL_INTERRUPT ||
			    data[0] == HALO_LUA_CTRL_RESTART || data[0] == HALO_LUA_CTRL_RESET ||
			    data[0] == HALO_LUA_CTRL_EXIT || data[0] == HALO_LUA_CTRL_REMOVE_ALL) {
				// clear REPL buffer
				ring_buf_reset(&lua_ctx.repl_rx_ring);
				// clear Data buffer
				ring_buf_reset(&lua_ctx.data_rx_ring);
				if (lua_ctx.ctrl_handler) {
					lua_ctx.ctrl_handler(data[0]);
				}
				break;
			}

			/* Route to data or REPL ring buffer */
			if (data[0] == HALO_LUA_CTRL_DATA_MARKER && len >= 2) {
				target_ring = &lua_ctx.data_rx_ring;
				target_sem = &lua_ctx.data_rx_sem;
			} else {
				target_ring = &lua_ctx.repl_rx_ring;
				target_sem = &lua_ctx.repl_rx_sem;
				len++; // reserve for '\n'
			}
		} else {
			/* Continuation of previous write - use last selected ring */
			if (lua_ctx.current_rx_ring == &lua_ctx.data_rx_ring) {
				target_ring = &lua_ctx.data_rx_ring;
				target_sem = &lua_ctx.data_rx_sem;
			} else {
				target_ring = &lua_ctx.repl_rx_ring;
				target_sem = &lua_ctx.repl_rx_sem;
				len++; // reserve for '\n'
			}
		}

		/* Update current ring for next continuation */
		lua_ctx.current_rx_ring = target_ring;

		/* Check buffer space */
		if (ring_buf_space_get(target_ring) < len) {
			LOG_WRN("RX ring buffer full (available=%u, needed=%u)",
				ring_buf_space_get(target_ring), len);
			status = ATT_ERR_INSUFF_RESOURCE;
			break;
		}

		/* Store data */
		if (target_ring == &lua_ctx.repl_rx_ring) {
			/* REPL: add data as-is */
			ring_buf_put(target_ring, data, len - 1);
			uint8_t newline = '\n';
			ring_buf_put(target_ring, &newline, 1);
			k_sem_give(target_sem);
		} else {
			/* Data: skip first byte (marker) on first write */
			if (offset == 0 && data[0] == HALO_LUA_CTRL_DATA_MARKER) {
				ring_buf_put(target_ring, data + 1, len - 1);
			} else {
				ring_buf_put(target_ring, data, len);
			}
			k_sem_give(target_sem);
		}
		break;
	}

	case LUA_IDX_TX_CCC: {
		if (len < 2) {
			status = ATT_ERR_INVALID_ATTRIBUTE_VAL_LEN;
			break;
		}
		uint16_t cfg = co_letoh16(co_read16(data));
		lua_ctx.tx_ccc_cfg = (cfg == GATT_CCC_START_NTF) ? cfg : GATT_CCC_STOP_NTFIND;
		break;
	}

	case LUA_IDX_VIDEO_CCC: {
		if (len < 2) {
			status = ATT_ERR_INVALID_ATTRIBUTE_VAL_LEN;
			break;
		}
		uint16_t cfg = co_letoh16(co_read16(data));
		lua_ctx.video_ccc_cfg = (cfg == GATT_CCC_START_NTF) ? cfg : GATT_CCC_STOP_NTFIND;
		break;
	}

	case LUA_IDX_AUDIO_RX_VAL: {
		/* Defence in depth: SEC_LVL(WP, UNAUTH) on this attribute already
		 * bars unencrypted writes at the controller, but mirror the RX_VAL
		 * pairing gate so an unpaired peer can never reach the audio path. */
		if (halo_ble_is_paired() == false) {
			LOG_WRN("Audio write attempt while not paired - rejected");
			status = ATT_ERR_INSUFF_AUTHEN;
			break;
		}
		/* Store audio input data */
		if (ring_buf_space_get(&lua_ctx.audio_rx_ring) < len) {
			status = ATT_ERR_INSUFF_RESOURCE;
			break;
		}
		ring_buf_put(&lua_ctx.audio_rx_ring, data, len);
		k_sem_give(&lua_ctx.audio_rx_sem);
		break;
	}

	case LUA_IDX_AUDIO_TX_CCC: {
		if (len < 2) {
			status = ATT_ERR_INVALID_ATTRIBUTE_VAL_LEN;
			break;
		}
		uint16_t cfg = co_letoh16(co_read16(data));
		lua_ctx.audio_tx_ccc_cfg = (cfg == GATT_CCC_START_NTF) ? cfg : GATT_CCC_STOP_NTFIND;
		break;
	}

	default:
		status = ATT_ERR_REQUEST_NOT_SUPPORTED;
		break;
	}

	gatt_srv_att_val_set_cfm(conidx, user_lid, token, status);
}

/* GATT event sent callback */
static void on_event_sent(uint8_t conidx, uint8_t user_lid, uint16_t metainfo, uint16_t status)
{
	if (status != GAP_ERR_NO_ERROR) {
		LOG_WRN("Notification failed: 0x%04X", status);
	}

	/* Release corresponding semaphore */
	switch (metainfo) {
	case LUA_METAINFO_TX_SEND:
		k_sem_give(&lua_ctx.tx_ntf_sem);
		break;
	case LUA_METAINFO_VIDEO_SEND:
		k_sem_give(&lua_ctx.video_ntf_sem);
		break;
	case LUA_METAINFO_AUDIO_SEND:
		k_sem_give(&lua_ctx.audio_ntf_sem);
		break;
	}
}

/* GATT service callbacks */
static const gatt_srv_cb_t gatt_cbs = {
	.cb_att_event_get = NULL,
	.cb_att_info_get = NULL,
	.cb_att_read_get = on_att_read_get,
	.cb_att_val_set = on_att_val_set,
	.cb_event_sent = on_event_sent,
};

/* BLE connection callback */
static int ble_lua_on_connected(uint8_t conidx)
{
	lua_ctx.conidx = conidx;

	if (halo_pm_is_sleeping()) {
		halo_pm_wakeup(HALO_PM_WAKEUP_BLE);
	}
	
	return 0;
}

/* BLE disconnection callback */
static int ble_lua_on_disconnected(uint8_t conidx)
{
	/* Deliberately NOT a wakeup source: a client commonly commands
	 * standby and then drops the link (e.g. to save phone battery), and
	 * that disconnect must not cancel the sleep it just requested. Wakes
	 * come from data writes, new connections, AAD, button, or timeout. */

	/* Reset state */
	lua_ctx.conidx = GAP_INVALID_CONIDX;
	lua_ctx.tx_ccc_cfg = GATT_CCC_STOP_NTFIND;
	lua_ctx.video_ccc_cfg = GATT_CCC_STOP_NTFIND;
	lua_ctx.audio_tx_ccc_cfg = GATT_CCC_STOP_NTFIND;

	/* Clear ring buffers */
	ring_buf_reset(&lua_ctx.repl_rx_ring);
	ring_buf_reset(&lua_ctx.data_rx_ring);
	ring_buf_reset(&lua_ctx.audio_rx_ring);

	/* Release waiting semaphores */
	k_sem_give(&lua_ctx.tx_ntf_sem);
	k_sem_give(&lua_ctx.video_ntf_sem);
	k_sem_give(&lua_ctx.audio_ntf_sem);
	

	return 0;
}

/* BLE service structure */
static struct halo_ble_service lua_service = {
	.name = "Lua Service",
	.init = NULL,
	.deinit = NULL,
	.on_connected = ble_lua_on_connected,
	.on_disconnected = ble_lua_on_disconnected,
	.on_paired = NULL,
};

/* Helper function for sending notifications */
static int32_t send_notification(const uint8_t *data, size_t len, uint16_t att_idx,
				  const uint16_t *ccc_cfg, struct k_sem *sem, uint16_t metainfo)
{
	if (lua_ctx.initialized != BLE_LUA_INIT_MAGIC) {
		return 0;
	}

	if (lua_ctx.conidx == GAP_INVALID_CONIDX || *ccc_cfg != GATT_CCC_START_NTF) {
		k_sleep(K_MSEC(10));
		return 0;
	}
	
	k_mutex_lock(&lua_ctx.write_lock, K_FOREVER);

	const uint8_t *p_data = data;
	size_t remaining = len;
	int32_t sent = 0;

	/* Chunk to the negotiated ATT payload, not the compile-time maximum. A
	 * notification value must fit in ATT_MTU-3; sizing to CFG_MAX_LE_MTU made
	 * every notification fail (and the transfer stall) whenever the peer
	 * negotiated a smaller MTU. halo_ble_get_mtu() already returns the usable
	 * payload for the live connection; fall back to the 20-byte default-MTU
	 * payload if it is momentarily unknown. */
	uint16_t max_payload = halo_ble_get_mtu();

	if (max_payload == 0) {
		max_payload = 20;
	}

	/* Send data in MTU-sized chunks */
	while (remaining > 0) {
		/* Wait for previous notification to complete */
		if (k_sem_take(sem, K_FOREVER) != 0) {
			break;
		}

		if (lua_ctx.initialized != BLE_LUA_INIT_MAGIC) {
			k_sem_give(sem);
			break;
		}

		/* Check if still connected and enabled */
		if (lua_ctx.conidx == GAP_INVALID_CONIDX || *ccc_cfg != GATT_CCC_START_NTF) {
			k_sem_give(sem);
			break;
		}

		alif_ble_mutex_lock(K_FOREVER);

		/* Calculate chunk size */
		size_t chunk_len = remaining > max_payload ? max_payload : remaining;

		/* Allocate buffer */
		co_buf_t *p_buf;
		uint16_t status = co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, chunk_len,
					       GATT_BUFFER_TAIL_LEN);
		if (status != CO_BUF_ERR_NO_ERROR) {
			LOG_ERR("Failed to allocate buffer");
			k_sem_give(sem);
			alif_ble_mutex_unlock();
			break;
		}

		/* Copy data and send */
		memcpy(co_buf_data(p_buf), p_data, chunk_len);
		status = gatt_srv_event_send(lua_ctx.conidx, lua_ctx.user_lid, metainfo,
					     GATT_NOTIFY, lua_ctx.start_hdl + att_idx, p_buf);

		co_buf_release(p_buf);
		alif_ble_mutex_unlock();

		if (status != GAP_ERR_NO_ERROR) {
			LOG_ERR("Failed to send notification: 0x%04X", status);
			k_sem_give(sem);
			break;
		}

		p_data += chunk_len;
		remaining -= chunk_len;
		sent += chunk_len;
	}

	k_mutex_unlock(&lua_ctx.write_lock);

	return sent;
}

/* Public API implementations */

int halo_ble_lua_init(bool reset)
{
	int ret;

	if (lua_ctx.initialized == BLE_LUA_INIT_MAGIC) {
		return 0;
	}

	LOG_DBG("Initializing BLE Lua Service");

	if (reset) {
		lua_ctx.conidx = GAP_INVALID_CONIDX;
		lua_ctx.tx_ccc_cfg = 0;
		lua_ctx.video_ccc_cfg = 0;
		lua_ctx.audio_tx_ccc_cfg = 0;
		lua_ctx.current_rx_ring = NULL;
		lua_ctx.ctrl_handler = NULL;
		lua_ctx.user_lid = GATT_INVALID_USER_LID;
		lua_ctx.start_hdl = GATT_INVALID_HDL;

		/* Register GATT user service */
		ret = gatt_user_srv_register(CFG_ATT_VAL_MAX, 0, &gatt_cbs, &lua_ctx.user_lid);
		if (ret != GAP_ERR_NO_ERROR) {
			LOG_ERR("Failed to register GATT user service: %d", ret);
			goto cleanup;
		}

		/* Add service to GATT database */
		ret = gatt_db_svc_add(lua_ctx.user_lid, SVC_UUID(128), lua_srv_uuid, LUA_IDX_NB,
				      NULL, lua_att_db, LUA_IDX_NB, &lua_ctx.start_hdl);
		if (ret != GAP_ERR_NO_ERROR) {
			LOG_ERR("Failed to add Lua service: %d", ret);
			goto cleanup;
		}
	}

	/* Allocate ring buffers using memory manager */
	lua_ctx.repl_rx_buf = halo_mem_alloc(CONFIG_HALO_LUA_MAX_REPL_SIZE + 1);
	if (!lua_ctx.repl_rx_buf) {
		LOG_ERR("Failed to allocate REPL buffer");
		return -ENOMEM;
	}

	lua_ctx.data_rx_buf = halo_mem_alloc(CONFIG_HALO_LUA_MAX_DATA_SIZE + 1);
	if (!lua_ctx.data_rx_buf) {
		LOG_ERR("Failed to allocate data buffer");
		halo_free(lua_ctx.repl_rx_buf);
		return -ENOMEM;
	}

	lua_ctx.audio_rx_buf = halo_mem_alloc(CONFIG_HALO_AUDIO_MAX_DATA_SIZE + 1);
	if (!lua_ctx.audio_rx_buf) {
		LOG_ERR("Failed to allocate audio buffer");
		halo_free(lua_ctx.repl_rx_buf);
		halo_free(lua_ctx.data_rx_buf);
		return -ENOMEM;
	}

	/* Initialize ring buffers */
	ring_buf_init(&lua_ctx.repl_rx_ring, CONFIG_HALO_LUA_MAX_REPL_SIZE + 1,
		      lua_ctx.repl_rx_buf);
	ring_buf_init(&lua_ctx.data_rx_ring, CONFIG_HALO_LUA_MAX_DATA_SIZE + 1,
		      lua_ctx.data_rx_buf);
	ring_buf_init(&lua_ctx.audio_rx_ring, CONFIG_HALO_AUDIO_MAX_DATA_SIZE + 1,
		      lua_ctx.audio_rx_buf);

	/* Initialize synchronization primitives */
	k_sem_init(&lua_ctx.repl_rx_sem, 0, 1);
	k_sem_init(&lua_ctx.data_rx_sem, 0, 1);
	k_sem_init(&lua_ctx.audio_rx_sem, 0, 1);
	k_sem_init(&lua_ctx.tx_ntf_sem, 1, 1);
	k_sem_init(&lua_ctx.video_ntf_sem, 1, 1);
	k_sem_init(&lua_ctx.audio_ntf_sem, 1, 1);
	k_mutex_init(&lua_ctx.write_lock);

	/* Register BLE service */
	ret = halo_ble_service_register(&lua_service);
	if (ret < 0) {
		LOG_ERR("Failed to register BLE service: %d", ret);
		goto cleanup;
	}

	lua_ctx.initialized = BLE_LUA_INIT_MAGIC;

	return 0;

cleanup:
	halo_free(lua_ctx.repl_rx_buf);
	halo_free(lua_ctx.data_rx_buf);
	halo_free(lua_ctx.audio_rx_buf);
	return -EIO;
}

int halo_ble_lua_deinit(void)
{
	if (lua_ctx.initialized != BLE_LUA_INIT_MAGIC) {
		return 0;
	}

	LOG_DBG("Deinitializing BLE Lua Service");

	/* Block new readers/writers immediately */
	lua_ctx.initialized = 0;

	/* Mark transport unavailable and release potential waiters */
	lua_ctx.conidx = GAP_INVALID_CONIDX;
	lua_ctx.tx_ccc_cfg = GATT_CCC_STOP_NTFIND;
	lua_ctx.video_ccc_cfg = GATT_CCC_STOP_NTFIND;
	lua_ctx.audio_tx_ccc_cfg = GATT_CCC_STOP_NTFIND;
	k_sem_give(&lua_ctx.repl_rx_sem);
	k_sem_give(&lua_ctx.data_rx_sem);
	k_sem_give(&lua_ctx.audio_rx_sem);
	k_sem_give(&lua_ctx.tx_ntf_sem);
	k_sem_give(&lua_ctx.video_ntf_sem);
	k_sem_give(&lua_ctx.audio_ntf_sem);

	halo_ble_service_unregister(&lua_service);

	/* Free allocated buffers */
	halo_free(lua_ctx.repl_rx_buf);
	halo_free(lua_ctx.data_rx_buf);
	halo_free(lua_ctx.audio_rx_buf);
	lua_ctx.repl_rx_buf = NULL;
	lua_ctx.data_rx_buf = NULL;
	lua_ctx.audio_rx_buf = NULL;

	return 0;
}

int32_t halo_ble_lua_repl_read(uint8_t *data, size_t len, k_timeout_t timeout)
{
	if (lua_ctx.initialized != BLE_LUA_INIT_MAGIC) {
		return 0;
	}

	if (k_sem_take(&lua_ctx.repl_rx_sem, timeout) != 0) {
		return 0;
	}

	uint32_t read_len = ring_buf_get(&lua_ctx.repl_rx_ring, data, len);

	/* If buffer not empty, keep semaphore available */
	if (!ring_buf_is_empty(&lua_ctx.repl_rx_ring)) {
		k_sem_give(&lua_ctx.repl_rx_sem);
	}

	return read_len;
}

int32_t halo_ble_lua_repl_write(const uint8_t *data, size_t len)
{
	if (lua_ctx.initialized != BLE_LUA_INIT_MAGIC) {
		return 0;
	}

	return send_notification(data, len, LUA_IDX_TX_VAL, &lua_ctx.tx_ccc_cfg, &lua_ctx.tx_ntf_sem,
				 LUA_METAINFO_TX_SEND);
}

int32_t halo_ble_lua_data_read(uint8_t *data, size_t len, k_timeout_t timeout)
{
	if (lua_ctx.initialized != BLE_LUA_INIT_MAGIC) {
		return 0;
	}

	if (k_sem_take(&lua_ctx.data_rx_sem, timeout) != 0) {
		return 0;
	}

	if (lua_ctx.initialized != BLE_LUA_INIT_MAGIC) {
		return 0;
	}

	uint32_t read_len = ring_buf_get(&lua_ctx.data_rx_ring, data, len);

	/* If buffer not empty, keep semaphore available */
	if (!ring_buf_is_empty(&lua_ctx.data_rx_ring)) {
		k_sem_give(&lua_ctx.data_rx_sem);
	}

	return read_len;
}

int32_t halo_ble_lua_data_write(const uint8_t *data, size_t len)
{
	if (lua_ctx.initialized != BLE_LUA_INIT_MAGIC) {
		return 0;
	}

	return send_notification(data, len, LUA_IDX_TX_VAL, &lua_ctx.tx_ccc_cfg, &lua_ctx.tx_ntf_sem,
				 LUA_METAINFO_TX_SEND);
}

int32_t halo_ble_lua_video_write(const uint8_t *data, size_t len)
{
	if (lua_ctx.initialized != BLE_LUA_INIT_MAGIC) {
		return 0;
	}

	return send_notification(data, len, LUA_IDX_VIDEO_VAL, &lua_ctx.video_ccc_cfg,
				 &lua_ctx.video_ntf_sem, LUA_METAINFO_VIDEO_SEND);
}

int32_t halo_ble_lua_audio_read(uint8_t *data, size_t len, k_timeout_t timeout)
{
	if (lua_ctx.initialized != BLE_LUA_INIT_MAGIC) {
		return 0;
	}

	if (k_sem_take(&lua_ctx.audio_rx_sem, timeout) != 0) {
		return 0;
	}

	if (lua_ctx.initialized != BLE_LUA_INIT_MAGIC) {
		return 0;
	}

	uint32_t read_len = ring_buf_get(&lua_ctx.audio_rx_ring, data, len);

	/* If buffer not empty, keep semaphore available */
	if (!ring_buf_is_empty(&lua_ctx.audio_rx_ring)) {
		k_sem_give(&lua_ctx.audio_rx_sem);
	}

	return read_len;
}

int32_t halo_ble_lua_audio_write(const uint8_t *data, size_t len)
{
	if (lua_ctx.initialized != BLE_LUA_INIT_MAGIC) {
		return 0;
	}

	return send_notification(data, len, LUA_IDX_AUDIO_TX_VAL, &lua_ctx.audio_tx_ccc_cfg,
				 &lua_ctx.audio_ntf_sem, LUA_METAINFO_AUDIO_SEND);
}

void halo_ble_lua_register_ctrl_handler(halo_ble_lua_ctrl_handler_t handler)
{
	lua_ctx.ctrl_handler = handler;
}

/* ============================================================================
 * Audio Transport Functions (audio_stream callback adapters)
 * ============================================================================ */

#ifdef CONFIG_HALO_AUDIO_STREAM
#include <halo/audio_stream.h>

static audio_source_handle_t ble_audio_source_handle = NULL;
static audio_sink_handle_t ble_audio_sink_handle = NULL;

/**
 * @brief Audio source callback adapter for BLE Lua service
 *
 * Allows BLE Lua service to act as an audio source for lua_speaker.
 */
static size_t ble_lua_audio_source_callback(uint8_t *data, size_t len, k_timeout_t timeout,
					    void *user_data)
{
	ARG_UNUSED(user_data);
	return halo_ble_lua_audio_read(data, len, timeout);
}

/**
 * @brief Audio sink callback adapter for BLE Lua service
 *
 * Allows BLE Lua service to act as an audio sink for lua_microphone.
 */
static size_t ble_lua_audio_sink_callback(const uint8_t *data, size_t len, void *user_data)
{
	ARG_UNUSED(user_data);
	return halo_ble_lua_audio_write(data, len);
}

/**
 * @brief Register BLE Lua audio transport with audio_stream
 *
 * Should be called during initialization to enable BLE audio transport
 * for lua_speaker and lua_microphone.
 *
 * @return 0 on success, negative errno on failure
 */
int halo_ble_lua_audio_transport_register(void)
{
	if (ble_audio_source_handle || ble_audio_sink_handle) {
		LOG_WRN("BLE audio transport already registered");
		return -EALREADY;
	}

	/* Register as audio source (for speaker) */
	ble_audio_source_handle =
		audio_stream_register_source(ble_lua_audio_source_callback, NULL, /* No user data */
					     10, /* Priority 10 (lower priority than LE Audio) */
					     "BLE Lua");

	if (!ble_audio_source_handle) {
		LOG_ERR("Failed to register BLE audio source");
		return -ENOMEM;
	}

	/* Register as audio sink (for microphone) */
	ble_audio_sink_handle =
		audio_stream_register_sink(ble_lua_audio_sink_callback, NULL, /* No user data */
					   10, /* Priority 10 (lower priority than LE Audio) */
					   "BLE Lua");

	if (!ble_audio_sink_handle) {
		LOG_ERR("Failed to register BLE audio sink");
		audio_stream_unregister_source(ble_audio_source_handle);
		ble_audio_source_handle = NULL;
		return -ENOMEM;
	}

	return 0;
}

/**
 * @brief Unregister BLE Lua audio transport
 *
 * Should be called during deinitialization.
 *
 * @return 0 on success, negative errno on failure
 */
int halo_ble_lua_audio_transport_unregister(void)
{
	int ret = 0;

	if (ble_audio_source_handle) {
		ret = audio_stream_unregister_source(ble_audio_source_handle);
		if (ret == 0) {
			ble_audio_source_handle = NULL;
		}
	}

	if (ble_audio_sink_handle) {
		int ret2 = audio_stream_unregister_sink(ble_audio_sink_handle);
		if (ret2 == 0) {
			ble_audio_sink_handle = NULL;
		}
		if (ret == 0) {
			ret = ret2;
		}
	}

	return ret;
}

#endif /* CONFIG_HALO_AUDIO_STREAM */
