/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * Apple Notification Center Service (ANCS) GATT client.
 *
 * iOS exposes ANCS as a GATT *server* on the phone; the accessory is the
 * client. Flow:
 *   connect -> discover ANCS by UUID -> (link encrypts) -> write CCCDs
 *   (Data Source first, then Notification Source) -> iOS replays existing
 *   notifications -> attribute fetches via Control Point / Data Source.
 *
 * All GATT callbacks run on the BLE host thread. Public API entry points
 * run on app threads (Lua REPL thread); shared state is protected by
 * ctx.lock, which is never held across a gatt_cli_*() call from an app
 * thread (the BLE host thread may block on ctx.lock inside a callback
 * while holding stack internals).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

#include "alif_ble.h"
#include "gatt.h"
#include "gatt_cli.h"
#include "gap_le.h"
#include "co_buf.h"
#include "co_endian.h"
#include "hl_error.h"

#include "halo/ble_ancs.h"
#include "halo/ble_manager.h"
#include "halo/ble_service.h"

LOG_MODULE_REGISTER(halo_ble_ancs, CONFIG_HALO_LOG_LEVEL);

/* ANCS UUIDs, LSB first as the RivieraWaves stack expects */
static const uint8_t ancs_svc_uuid[GATT_UUID_128_LEN] = {
	0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4,
	0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79};
static const uint8_t ancs_ns_uuid[GATT_UUID_128_LEN] = {
	0xBD, 0x1D, 0xA2, 0x99, 0xE6, 0x25, 0x58, 0x8C,
	0xD9, 0x42, 0x01, 0x63, 0x0D, 0x12, 0xBF, 0x9F};
static const uint8_t ancs_cp_uuid[GATT_UUID_128_LEN] = {
	0xD9, 0xD9, 0xAA, 0xFD, 0xBD, 0x9B, 0x21, 0x98,
	0xA8, 0x49, 0xE1, 0x45, 0xF3, 0xD8, 0xD1, 0x69};
static const uint8_t ancs_ds_uuid[GATT_UUID_128_LEN] = {
	0xFB, 0x7B, 0x7C, 0xCE, 0x6A, 0xB3, 0x44, 0xBE,
	0xB5, 0x4B, 0xD6, 0x24, 0xE9, 0xC6, 0xEA, 0x22};

/* ANCS Control Point command IDs */
#define ANCS_CMD_GET_NOTIF_ATTRS 0
#define ANCS_CMD_GET_APP_ATTRS   1
#define ANCS_CMD_PERFORM_ACTION  2

/* Notification Source event payload is always 8 bytes */
#define ANCS_NS_EVENT_LEN 8

/* Maximum encoded Control Point command:
 * GetAppAttributes: cmd(1) + app id + NUL + attr id(1) */
#define ANCS_APP_ID_MAX_LEN 160
#define ANCS_CMD_BUF_LEN    (1 + ANCS_APP_ID_MAX_LEN + 1 + 1)

/* metainfo tags for in-flight GATT procedures */
enum ancs_proc {
	ANCS_PROC_NONE = 0,
	ANCS_PROC_DISCOVER,
	ANCS_PROC_SUB_DS,       /* write Data Source CCCD */
	ANCS_PROC_SUB_NS_RESET, /* write Notification Source CCCD = 0 pre-subscribe */
	ANCS_PROC_SUB_NS,       /* write Notification Source CCCD = 1 */
	ANCS_PROC_UNSUB_NS,
	ANCS_PROC_UNSUB_DS,
	ANCS_PROC_CMD,          /* write Control Point */
};

/* Discovery / subscription state */
enum ancs_state {
	ANCS_STATE_IDLE = 0,    /* no connection */
	ANCS_STATE_CONNECTED,   /* connected, discovery not finished */
	ANCS_STATE_DISCOVERING,
	ANCS_STATE_UNAVAILABLE, /* discovery done, no (complete) ANCS found */
	ANCS_STATE_AVAILABLE,   /* handles known */
};

enum ancs_sub_state {
	ANCS_SUB_NONE = 0,
	ANCS_SUB_PENDING,     /* want subscription, not started/failed-retrying */
	ANCS_SUB_IN_PROGRESS, /* CCCD write(s) in flight */
	ANCS_SUB_SUBSCRIBED,
};

/* Control Point command state */
enum ancs_cmd_state {
	ANCS_CMD_IDLE = 0,
	ANCS_CMD_WRITING,      /* Control Point write in flight */
	ANCS_CMD_WAIT_RESPONSE /* waiting for Data Source response */
};

#define ANCS_SUB_RETRY_MS       1000 /* CCCD retry while link not yet encrypted */
#define ANCS_SUB_RETRY_MAX      30
#define ANCS_DISC_RETRY_MS      2000 /* discovery retry on transient failure */
#define ANCS_DISC_RETRY_MAX     5
#define ANCS_RESPONSE_TIMEOUT_S 15   /* Data Source response timeout */

struct ancs_ctx {
	bool initialized;
	uint8_t user_lid;
	bool user_registered;

	struct halo_ancs_callbacks cbs;

	enum ancs_state state;
	uint8_t conidx;
	bool enabled; /* app wants Notification Source events */
	enum ancs_sub_state sub_state;
	uint8_t sub_retries;
	uint8_t disc_retries;
	bool rediscover; /* service changed while discovery in flight */

	/* Discovered handles */
	uint16_t svc_start_hdl;
	uint16_t svc_end_hdl;
	uint16_t ns_val_hdl;
	uint16_t ns_cccd_hdl;
	uint16_t cp_val_hdl;
	uint16_t ds_val_hdl;
	uint16_t ds_cccd_hdl;
	/* Track which characteristic declaration was seen last during
	 * discovery, so following descriptors are attributed correctly
	 * (discovery data may arrive split across several cb_svc calls). */
	uint8_t disc_last_char; /* 0 none, 1 NS, 2 CP, 3 DS */

	/* In-flight Control Point command */
	enum ancs_cmd_state cmd_state;
	uint8_t cmd_id;
	uint32_t cmd_uid;      /* GetNotifAttrs / PerformAction */
	uint8_t cmd_action_id; /* PerformAction */
	uint8_t cmd_attr_mask; /* GetNotifAttrs: requested attribute bitmask */
	uint8_t cmd_attr_count;
	char cmd_app_id[ANCS_APP_ID_MAX_LEN + 1]; /* GetAppAttrs */

	/* Data Source response accumulator */
	uint8_t resp[CONFIG_HALO_BLE_ANCS_RESPONSE_BUF_SIZE];
	uint16_t resp_len;

	struct k_work_delayable sub_retry_work;
	struct k_work_delayable disc_retry_work;
	struct k_work_delayable resp_timeout_work;
};

static struct ancs_ctx ctx = {
	.conidx = GAP_INVALID_CONIDX,
	.user_lid = GATT_INVALID_USER_LID,
};

/* Statically initialized so halo_ble_ancs_set_callbacks() is safe to call
 * before halo_ble_ancs_init(). Zephyr mutexes are recursive, so callbacks
 * invoked under the lock may re-enter the public API. */
K_MUTEX_DEFINE(ancs_lock);

/* Forward declarations */
static void ancs_start_discovery(void);
static void ancs_try_subscribe(void);
static void ancs_finish_command(int status);

/* ---------------------------------------------------------------------- */
/* Helpers                                                                 */
/* ---------------------------------------------------------------------- */

static bool uuid128_match(uint8_t uuid_type, const uint8_t *p_uuid, const uint8_t *ref)
{
	return uuid_type == GATT_UUID_128 && memcmp(p_uuid, ref, GATT_UUID_128_LEN) == 0;
}

static bool uuid_is_cccd(uint8_t uuid_type, const uint8_t *p_uuid)
{
	return uuid_type == GATT_UUID_16 &&
	       p_uuid[0] == (GATT_DESC_CLIENT_CHAR_CFG & 0xFF) &&
	       p_uuid[1] == (GATT_DESC_CLIENT_CHAR_CFG >> 8);
}

static bool handles_complete(void)
{
	return ctx.ns_val_hdl != GATT_INVALID_HDL && ctx.ns_cccd_hdl != GATT_INVALID_HDL &&
	       ctx.cp_val_hdl != GATT_INVALID_HDL && ctx.ds_val_hdl != GATT_INVALID_HDL &&
	       ctx.ds_cccd_hdl != GATT_INVALID_HDL;
}

static void reset_handles(void)
{
	ctx.svc_start_hdl = GATT_INVALID_HDL;
	ctx.svc_end_hdl = GATT_INVALID_HDL;
	ctx.ns_val_hdl = GATT_INVALID_HDL;
	ctx.ns_cccd_hdl = GATT_INVALID_HDL;
	ctx.cp_val_hdl = GATT_INVALID_HDL;
	ctx.ds_val_hdl = GATT_INVALID_HDL;
	ctx.ds_cccd_hdl = GATT_INVALID_HDL;
	ctx.disc_last_char = 0;
}

static void notify_availability(bool available)
{
	k_mutex_lock(&ancs_lock, K_FOREVER);
	if (ctx.cbs.availability) {
		ctx.cbs.availability(available, ctx.cbs.user_data);
	}
	k_mutex_unlock(&ancs_lock);
}

/* ATT errors that mean "not yet encrypted/authenticated" - retry later */
static bool status_needs_encryption(uint16_t status)
{
	return status == ATT_ERR_INSUFF_AUTHEN || status == ATT_ERR_INSUFF_ENC ||
	       status == ATT_ERR_INSUFF_ENC_KEY_SIZE || status == ATT_ERR_INSUFF_AUTHOR;
}

/* Write a 2-byte CCCD value. Caller must NOT hold ctx.lock. */
static uint16_t ancs_write_cccd(uint8_t conidx, uint16_t hdl, uint16_t value, uint16_t metainfo)
{
	co_buf_t *p_buf;
	uint16_t err;

	err = co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, 2, GATT_BUFFER_TAIL_LEN);
	if (err != CO_BUF_ERR_NO_ERROR) {
		return GAP_ERR_INSUFF_RESOURCES;
	}

	co_write16(co_buf_data(p_buf), co_htole16(value));

	alif_ble_mutex_lock(K_FOREVER);
	err = gatt_cli_write(conidx, ctx.user_lid, metainfo, GATT_WRITE, hdl, 0, p_buf);
	alif_ble_mutex_unlock();

	co_buf_release(p_buf);
	return err;
}

/* ---------------------------------------------------------------------- */
/* Discovery                                                               */
/* ---------------------------------------------------------------------- */

/* Kick ANCS service discovery. Caller must NOT hold ctx.lock. */
static void ancs_start_discovery(void)
{
	uint8_t conidx;

	k_mutex_lock(&ancs_lock, K_FOREVER);
	if (ctx.state != ANCS_STATE_CONNECTED || ctx.conidx == GAP_INVALID_CONIDX) {
		k_mutex_unlock(&ancs_lock);
		return;
	}
	ctx.state = ANCS_STATE_DISCOVERING;
	ctx.rediscover = false;
	reset_handles();
	conidx = ctx.conidx;
	k_mutex_unlock(&ancs_lock);

	alif_ble_mutex_lock(K_FOREVER);
	uint16_t err = gatt_cli_discover_svc(conidx, ctx.user_lid, ANCS_PROC_DISCOVER,
					     GATT_DISCOVER_SVC_PRIMARY_BY_UUID, true,
					     GATT_MIN_HDL, GATT_MAX_HDL, GATT_UUID_128,
					     ancs_svc_uuid);
	alif_ble_mutex_unlock();

	if (err != GAP_ERR_NO_ERROR) {
		LOG_WRN("ANCS discovery start failed: 0x%04x", err);
		k_mutex_lock(&ancs_lock, K_FOREVER);
		if (ctx.state == ANCS_STATE_DISCOVERING) {
			ctx.state = ANCS_STATE_CONNECTED;
			if (ctx.disc_retries < ANCS_DISC_RETRY_MAX) {
				ctx.disc_retries++;
				k_work_schedule(&ctx.disc_retry_work, K_MSEC(ANCS_DISC_RETRY_MS));
			} else {
				ctx.state = ANCS_STATE_UNAVAILABLE;
			}
		}
		k_mutex_unlock(&ancs_lock);
	}
}

/* Consume one attribute-info entry from a full-service discovery result */
static void disc_consume_att(uint16_t hdl, const gatt_svc_att_t *att)
{
	switch (att->att_type) {
	case GATT_ATT_CHAR:
		if (uuid128_match(att->uuid_type, att->uuid, ancs_ns_uuid)) {
			ctx.ns_val_hdl = att->info.charac.val_hdl;
			ctx.disc_last_char = 1;
		} else if (uuid128_match(att->uuid_type, att->uuid, ancs_cp_uuid)) {
			ctx.cp_val_hdl = att->info.charac.val_hdl;
			ctx.disc_last_char = 2;
		} else if (uuid128_match(att->uuid_type, att->uuid, ancs_ds_uuid)) {
			ctx.ds_val_hdl = att->info.charac.val_hdl;
			ctx.disc_last_char = 3;
		} else {
			ctx.disc_last_char = 0;
		}
		break;

	case GATT_ATT_DESC:
		if (uuid_is_cccd(att->uuid_type, att->uuid)) {
			if (ctx.disc_last_char == 1) {
				ctx.ns_cccd_hdl = hdl;
			} else if (ctx.disc_last_char == 3) {
				ctx.ds_cccd_hdl = hdl;
			}
		}
		break;

	default:
		break;
	}
}

static void on_svc(uint8_t conidx, uint8_t user_lid, uint16_t metainfo, uint16_t hdl,
		   uint8_t disc_info, uint8_t nb_att, const gatt_svc_att_t *p_atts)
{
	ARG_UNUSED(user_lid);
	ARG_UNUSED(metainfo);

	k_mutex_lock(&ancs_lock, K_FOREVER);

	if (ctx.state != ANCS_STATE_DISCOVERING || conidx != ctx.conidx) {
		k_mutex_unlock(&ancs_lock);
		return;
	}

	if (disc_info == GATT_SVC_CMPLT || disc_info == GATT_SVC_START) {
		/* First attribute of the structure is the service declaration */
		ctx.svc_start_hdl = hdl;
		ctx.disc_last_char = 0;
	}

	for (uint8_t i = 0; i < nb_att; i++) {
		disc_consume_att(hdl + i, &p_atts[i]);
	}

	if (hdl + nb_att - 1 > ctx.svc_end_hdl || ctx.svc_end_hdl == GATT_INVALID_HDL) {
		ctx.svc_end_hdl = hdl + nb_att - 1;
	}

	k_mutex_unlock(&ancs_lock);
}

static void on_discover_cmp(uint8_t conidx, uint8_t user_lid, uint16_t metainfo, uint16_t status)
{
	ARG_UNUSED(user_lid);
	ARG_UNUSED(metainfo);

	bool available = false;
	bool retry = false;
	bool subscribe = false;
	bool rediscover = false;

	k_mutex_lock(&ancs_lock, K_FOREVER);

	if (ctx.state != ANCS_STATE_DISCOVERING || conidx != ctx.conidx) {
		k_mutex_unlock(&ancs_lock);
		return;
	}

	if (ctx.rediscover) {
		/* Service changed while we were discovering - start over */
		ctx.state = ANCS_STATE_CONNECTED;
		rediscover = true;
	} else if (status == GAP_ERR_NO_ERROR && handles_complete()) {
		ctx.state = ANCS_STATE_AVAILABLE;
		ctx.disc_retries = 0;
		available = true;
		if (ctx.enabled) {
			ctx.sub_state = ANCS_SUB_PENDING;
			ctx.sub_retries = 0;
			subscribe = true;
		}
	} else if (status == GAP_ERR_NO_ERROR || status == ATT_ERR_ATTRIBUTE_NOT_FOUND) {
		/* Peer has no (complete) ANCS - e.g. Android central. iOS can
		 * publish the service later; a Service Changed indication will
		 * trigger rediscovery then. */
		ctx.state = ANCS_STATE_UNAVAILABLE;
		LOG_INF("ANCS not present on peer");
	} else {
		LOG_WRN("ANCS discovery failed: 0x%04x", status);
		ctx.state = ANCS_STATE_CONNECTED;
		if (ctx.disc_retries < ANCS_DISC_RETRY_MAX) {
			ctx.disc_retries++;
			retry = true;
		} else {
			ctx.state = ANCS_STATE_UNAVAILABLE;
		}
	}

	uint16_t start_hdl = ctx.svc_start_hdl;
	uint16_t end_hdl = ctx.svc_end_hdl;
	k_mutex_unlock(&ancs_lock);

	if (available) {
		LOG_INF("ANCS discovered: svc [0x%04x-0x%04x] ns=0x%04x cp=0x%04x ds=0x%04x",
			start_hdl, end_hdl, ctx.ns_val_hdl, ctx.cp_val_hdl, ctx.ds_val_hdl);

		/* Register for notifications on the whole service range */
		alif_ble_mutex_lock(K_FOREVER);
		uint16_t err = gatt_cli_event_register(conidx, ctx.user_lid, start_hdl, end_hdl);
		alif_ble_mutex_unlock();
		if (err != GAP_ERR_NO_ERROR) {
			LOG_ERR("ANCS event register failed: 0x%04x", err);
		}

		notify_availability(true);
		if (subscribe) {
			ancs_try_subscribe();
		}
	} else if (retry) {
		k_work_schedule(&ctx.disc_retry_work, K_MSEC(ANCS_DISC_RETRY_MS));
	} else if (rediscover) {
		ancs_start_discovery();
	}
}

/* ---------------------------------------------------------------------- */
/* Subscription (CCCD writes)                                              */
/* ---------------------------------------------------------------------- */

/* Begin/continue subscribing: Data Source first, then Notification Source
 * (Apple-recommended order, so attribute responses can be received before
 * the first Notification Source event arrives).
 *
 * The Notification Source CCCD is written 0 then 1: iOS replays the
 * existing notifications only on a 0->1 transition, and it persists CCCD
 * state for bonded peers - if a previous session (or a failed
 * unsubscribe) left the CCCD at 1, a plain enable would silently deliver
 * no replay, only live events.
 * Caller must NOT hold ctx.lock. */
static void ancs_try_subscribe(void)
{
	uint8_t conidx;
	uint16_t hdl;

	k_mutex_lock(&ancs_lock, K_FOREVER);
	if (ctx.state != ANCS_STATE_AVAILABLE || !ctx.enabled ||
	    ctx.sub_state == ANCS_SUB_SUBSCRIBED || ctx.sub_state == ANCS_SUB_IN_PROGRESS) {
		k_mutex_unlock(&ancs_lock);
		return;
	}

	/* Do not subscribe until the link is actually encrypted. iOS enforces
	 * this by rejecting the CCCD write with INSUFF_ENC, but an untrusted peer
	 * presenting a fake ANCS service will happily accept the subscription over
	 * an unencrypted link and then stream forged notification content to the
	 * display. Gate locally: stay PENDING and retry - the
	 * HALO_BLE_EVENT_ENCRYPTED nudge (or the retry timer) re-drives this once
	 * encryption completes. */
	if (!halo_ble_is_encrypted()) {
		ctx.sub_state = ANCS_SUB_PENDING;
		if (ctx.sub_retries < ANCS_SUB_RETRY_MAX) {
			ctx.sub_retries++;
			k_work_schedule(&ctx.sub_retry_work, K_MSEC(ANCS_SUB_RETRY_MS));
		} else {
			LOG_ERR("ANCS subscription gave up waiting for encryption");
		}
		k_mutex_unlock(&ancs_lock);
		return;
	}

	ctx.sub_state = ANCS_SUB_IN_PROGRESS;
	conidx = ctx.conidx;
	hdl = ctx.ds_cccd_hdl;
	k_mutex_unlock(&ancs_lock);

	uint16_t err = ancs_write_cccd(conidx, hdl, GATT_CCC_START_NTF, ANCS_PROC_SUB_DS);
	if (err != GAP_ERR_NO_ERROR) {
		LOG_WRN("ANCS DS CCCD write failed to start: 0x%04x", err);
		k_mutex_lock(&ancs_lock, K_FOREVER);
		if (ctx.sub_state == ANCS_SUB_IN_PROGRESS) {
			ctx.sub_state = ANCS_SUB_PENDING;
			k_work_schedule(&ctx.sub_retry_work, K_MSEC(ANCS_SUB_RETRY_MS));
		}
		k_mutex_unlock(&ancs_lock);
	}
}

/* Handle completion of a subscription-related CCCD write */
static void on_sub_write_cmp(uint8_t conidx, uint16_t metainfo, uint16_t status)
{
	uint16_t write_proc = ANCS_PROC_NONE;
	uint16_t write_value = 0;
	bool retry = false;
	bool subscribed = false;
	uint16_t ns_hdl = GATT_INVALID_HDL;

	k_mutex_lock(&ancs_lock, K_FOREVER);

	if (conidx != ctx.conidx || ctx.sub_state != ANCS_SUB_IN_PROGRESS) {
		k_mutex_unlock(&ancs_lock);
		return;
	}

	if (status == GAP_ERR_NO_ERROR) {
		if (metainfo == ANCS_PROC_SUB_DS) {
			write_proc = ANCS_PROC_SUB_NS_RESET;
			write_value = GATT_CCC_STOP_NTFIND;
			ns_hdl = ctx.ns_cccd_hdl;
		} else if (metainfo == ANCS_PROC_SUB_NS_RESET) {
			write_proc = ANCS_PROC_SUB_NS;
			write_value = GATT_CCC_START_NTF;
			ns_hdl = ctx.ns_cccd_hdl;
		} else { /* ANCS_PROC_SUB_NS */
			ctx.sub_state = ANCS_SUB_SUBSCRIBED;
			ctx.sub_retries = 0;
			subscribed = true;
		}
	} else if (status_needs_encryption(status)) {
		/* Link not yet encrypted - iOS pairs/encrypts asynchronously */
		ctx.sub_state = ANCS_SUB_PENDING;
		if (ctx.sub_retries < ANCS_SUB_RETRY_MAX) {
			ctx.sub_retries++;
			retry = true;
		} else {
			LOG_ERR("ANCS subscription gave up waiting for encryption");
		}
	} else {
		LOG_WRN("ANCS CCCD write failed: 0x%04x", status);
		ctx.sub_state = ANCS_SUB_PENDING;
		if (ctx.sub_retries < ANCS_SUB_RETRY_MAX) {
			ctx.sub_retries++;
			retry = true;
		}
	}

	k_mutex_unlock(&ancs_lock);

	if (write_proc != ANCS_PROC_NONE) {
		uint16_t err = ancs_write_cccd(conidx, ns_hdl, write_value, write_proc);
		if (err != GAP_ERR_NO_ERROR) {
			k_mutex_lock(&ancs_lock, K_FOREVER);
			if (ctx.sub_state == ANCS_SUB_IN_PROGRESS) {
				ctx.sub_state = ANCS_SUB_PENDING;
				k_work_schedule(&ctx.sub_retry_work, K_MSEC(ANCS_SUB_RETRY_MS));
			}
			k_mutex_unlock(&ancs_lock);
		}
	} else if (retry) {
		k_work_schedule(&ctx.sub_retry_work, K_MSEC(ANCS_SUB_RETRY_MS));
	} else if (subscribed) {
		LOG_INF("ANCS subscribed");
	}
}

/* Unsubscribe (best effort, fire-and-forget). Caller must NOT hold ctx.lock. */
static void ancs_unsubscribe(uint8_t conidx, uint16_t ns_cccd, uint16_t ds_cccd)
{
	if (conidx == GAP_INVALID_CONIDX) {
		return;
	}
	if (ns_cccd != GATT_INVALID_HDL) {
		ancs_write_cccd(conidx, ns_cccd, GATT_CCC_STOP_NTFIND, ANCS_PROC_UNSUB_NS);
	}
	if (ds_cccd != GATT_INVALID_HDL) {
		ancs_write_cccd(conidx, ds_cccd, GATT_CCC_STOP_NTFIND, ANCS_PROC_UNSUB_DS);
	}
}

/* ---------------------------------------------------------------------- */
/* Control Point commands / Data Source responses                          */
/* ---------------------------------------------------------------------- */

/* Number of attributes that will be requested for a mask/request */
static uint8_t attr_request_count(const struct halo_ancs_attr_request *req)
{
	uint8_t count = 0;

	for (uint8_t id = 0; id < HALO_ANCS_ATTR_COUNT; id++) {
		if (!(req->attr_mask & BIT(id))) {
			continue;
		}
		if ((id == HALO_ANCS_ATTR_TITLE && req->title_max == 0) ||
		    (id == HALO_ANCS_ATTR_SUBTITLE && req->subtitle_max == 0) ||
		    (id == HALO_ANCS_ATTR_MESSAGE && req->message_max == 0)) {
			continue;
		}
		count++;
	}
	return count;
}

/* Send a Control Point command that is already staged in ctx (cmd_*).
 * Encodes and writes; on immediate failure resets cmd state.
 * Caller must NOT hold ctx.lock; cmd_state must be ANCS_CMD_WRITING. */
static int ancs_send_command(uint8_t conidx, uint16_t cp_hdl,
			     const struct halo_ancs_attr_request *req)
{
	uint8_t buf[ANCS_CMD_BUF_LEN];
	uint16_t len = 0;

	buf[len++] = ctx.cmd_id;

	switch (ctx.cmd_id) {
	case ANCS_CMD_GET_NOTIF_ATTRS:
		co_write32(&buf[len], co_htole32(ctx.cmd_uid));
		len += 4;
		for (uint8_t id = 0; id < HALO_ANCS_ATTR_COUNT; id++) {
			if (!(req->attr_mask & BIT(id))) {
				continue;
			}
			uint16_t max = 0;

			if (id == HALO_ANCS_ATTR_TITLE) {
				max = req->title_max;
			} else if (id == HALO_ANCS_ATTR_SUBTITLE) {
				max = req->subtitle_max;
			} else if (id == HALO_ANCS_ATTR_MESSAGE) {
				max = req->message_max;
			}

			if ((id == HALO_ANCS_ATTR_TITLE || id == HALO_ANCS_ATTR_SUBTITLE ||
			     id == HALO_ANCS_ATTR_MESSAGE)) {
				if (max == 0) {
					continue;
				}
				buf[len++] = id;
				co_write16(&buf[len], co_htole16(max));
				len += 2;
			} else {
				buf[len++] = id;
			}
		}
		break;

	case ANCS_CMD_GET_APP_ATTRS: {
		size_t id_len = strlen(ctx.cmd_app_id);

		memcpy(&buf[len], ctx.cmd_app_id, id_len + 1); /* include NUL */
		len += id_len + 1;
		buf[len++] = HALO_ANCS_APP_ATTR_DISPLAY_NAME;
		break;
	}

	case ANCS_CMD_PERFORM_ACTION:
		co_write32(&buf[len], co_htole32(ctx.cmd_uid));
		len += 4;
		buf[len++] = ctx.cmd_action_id;
		break;

	default:
		return -EINVAL;
	}

	co_buf_t *p_buf;
	uint16_t err = co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, len, GATT_BUFFER_TAIL_LEN);

	if (err != CO_BUF_ERR_NO_ERROR) {
		ancs_finish_command(-ENOMEM);
		return -ENOMEM;
	}
	memcpy(co_buf_data(p_buf), buf, len);

	alif_ble_mutex_lock(K_FOREVER);
	err = gatt_cli_write(conidx, ctx.user_lid, ANCS_PROC_CMD, GATT_WRITE, cp_hdl, 0, p_buf);
	alif_ble_mutex_unlock();
	co_buf_release(p_buf);

	if (err != GAP_ERR_NO_ERROR) {
		LOG_WRN("ANCS control point write failed: 0x%04x", err);
		ancs_finish_command(-EIO);
		return -EIO;
	}

	return 0;
}

/* Deliver the in-flight command's result with the given status and reset to
 * idle. status 0 means "parse the accumulated response". May be called from
 * BLE thread, timeout work or (on send failure) an app thread.
 * Caller must NOT hold ctx.lock. */
static void ancs_finish_command(int status)
{
	k_mutex_lock(&ancs_lock, K_FOREVER);

	if (ctx.cmd_state == ANCS_CMD_IDLE) {
		k_mutex_unlock(&ancs_lock);
		return;
	}

	uint8_t cmd_id = ctx.cmd_id;
	uint32_t uid = ctx.cmd_uid;
	uint8_t action_id = ctx.cmd_action_id;

	ctx.cmd_state = ANCS_CMD_IDLE;
	k_work_cancel_delayable(&ctx.resp_timeout_work);

	/* Parse response / build result while holding the lock (resp buffer
	 * is only touched by the BLE thread, but guard against re-entry). */
	switch (cmd_id) {
	case ANCS_CMD_GET_NOTIF_ATTRS: {
		struct halo_ancs_attr_result result = {
			.uid = uid,
			.status = status,
		};

		if (status == 0) {
			/* resp: cmd(1) uid(4) [id(1) len(2) data]xN - already
			 * validated complete by the reassembler */
			uint16_t off = 5;

			while (off + 3 <= ctx.resp_len &&
			       result.attr_count < HALO_ANCS_ATTR_COUNT) {
				uint8_t id = ctx.resp[off];
				uint16_t alen = co_letoh16(co_read16(&ctx.resp[off + 1]));

				off += 3;
				if (off + alen > ctx.resp_len) {
					break;
				}
				result.attrs[result.attr_count++] = (struct halo_ancs_attr){
					.id = id,
					.length = alen,
					.data = &ctx.resp[off],
				};
				off += alen;
			}
		}

		if (ctx.cbs.attributes) {
			ctx.cbs.attributes(&result, ctx.cbs.user_data);
		}
		break;
	}

	case ANCS_CMD_GET_APP_ATTRS: {
		struct halo_ancs_app_attr_result result = {
			.app_id = ctx.cmd_app_id,
			.status = status,
		};

		if (status == 0) {
			/* resp: cmd(1) app_id(NUL-terminated) [id(1) len(2) data]xN */
			uint16_t off = 1;

			while (off < ctx.resp_len && ctx.resp[off] != 0) {
				off++;
			}
			off++; /* skip NUL */

			while (off + 3 <= ctx.resp_len &&
			       result.attr_count < HALO_ANCS_APP_ATTR_COUNT) {
				uint8_t id = ctx.resp[off];
				uint16_t alen = co_letoh16(co_read16(&ctx.resp[off + 1]));

				off += 3;
				if (off + alen > ctx.resp_len) {
					break;
				}
				result.attrs[result.attr_count++] = (struct halo_ancs_attr){
					.id = id,
					.length = alen,
					.data = &ctx.resp[off],
				};
				off += alen;
			}
		}

		if (ctx.cbs.app_attributes) {
			ctx.cbs.app_attributes(&result, ctx.cbs.user_data);
		}
		break;
	}

	case ANCS_CMD_PERFORM_ACTION: {
		struct halo_ancs_action_result result = {
			.uid = uid,
			.action_id = action_id,
			.status = status,
		};

		if (ctx.cbs.action) {
			ctx.cbs.action(&result, ctx.cbs.user_data);
		}
		break;
	}

	default:
		break;
	}

	ctx.resp_len = 0;
	k_mutex_unlock(&ancs_lock);
}

/* Check whether the accumulated Data Source response is complete.
 * Called with ctx.lock held. Returns true when complete. */
static bool response_is_complete(void)
{
	uint16_t off;
	uint8_t tlv_count = 0;
	uint8_t expected;

	if (ctx.resp_len < 1 || ctx.resp[0] != ctx.cmd_id) {
		return false;
	}

	switch (ctx.cmd_id) {
	case ANCS_CMD_GET_NOTIF_ATTRS:
		if (ctx.resp_len < 5) {
			return false;
		}
		if (co_letoh32(co_read32(&ctx.resp[1])) != ctx.cmd_uid) {
			return false;
		}
		off = 5;
		expected = ctx.cmd_attr_count;
		break;

	case ANCS_CMD_GET_APP_ATTRS:
		off = 1;
		while (off < ctx.resp_len && ctx.resp[off] != 0) {
			off++;
		}
		if (off >= ctx.resp_len) {
			return false; /* NUL terminator not yet received */
		}
		off++;
		expected = 1; /* display name */
		break;

	default:
		return false;
	}

	while (tlv_count < expected) {
		if (off + 3 > ctx.resp_len) {
			return false;
		}
		uint16_t alen = co_letoh16(co_read16(&ctx.resp[off + 1]));

		off += 3 + alen;
		if (off > ctx.resp_len) {
			return false;
		}
		tlv_count++;
	}

	return true;
}

/* Feed Data Source notification bytes into the reassembler.
 * Called from the BLE thread. */
static void ancs_ds_data(const uint8_t *data, uint16_t len)
{
	bool complete = false;
	bool overflow = false;

	k_mutex_lock(&ancs_lock, K_FOREVER);

	if (ctx.cmd_state != ANCS_CMD_WAIT_RESPONSE) {
		k_mutex_unlock(&ancs_lock);
		LOG_DBG("ANCS DS data with no command in flight (%u bytes)", len);
		return;
	}

	if (ctx.resp_len + len > sizeof(ctx.resp)) {
		overflow = true;
	} else {
		memcpy(&ctx.resp[ctx.resp_len], data, len);
		ctx.resp_len += len;
		complete = response_is_complete();
	}

	k_mutex_unlock(&ancs_lock);

	if (overflow) {
		LOG_ERR("ANCS response exceeds %u byte buffer", sizeof(ctx.resp));
		ancs_finish_command(-EMSGSIZE);
	} else if (complete) {
		ancs_finish_command(0);
	}
}

/* Control Point write completion */
static void on_cmd_write_cmp(uint8_t conidx, uint16_t status)
{
	bool wait_response = false;
	int fail_status = 0;

	k_mutex_lock(&ancs_lock, K_FOREVER);

	if (conidx != ctx.conidx || ctx.cmd_state != ANCS_CMD_WRITING) {
		k_mutex_unlock(&ancs_lock);
		return;
	}

	if (status == GAP_ERR_NO_ERROR) {
		if (ctx.cmd_id == ANCS_CMD_PERFORM_ACTION) {
			/* No Data Source response follows an action */
			k_mutex_unlock(&ancs_lock);
			ancs_finish_command(0);
			return;
		}
		ctx.cmd_state = ANCS_CMD_WAIT_RESPONSE;
		ctx.resp_len = 0;
		wait_response = true;
	} else if (status >= HALO_ANCS_ERR_UNKNOWN_COMMAND &&
		   status <= HALO_ANCS_ERR_ACTION_FAILED) {
		/* ANCS application error (e.g. Invalid Parameter for a
		 * notification that has since been dismissed) */
		fail_status = status;
	} else if (status_needs_encryption(status)) {
		fail_status = -EACCES;
	} else {
		LOG_WRN("ANCS control point write completed with 0x%04x", status);
		fail_status = -EIO;
	}

	k_mutex_unlock(&ancs_lock);

	if (wait_response) {
		k_work_schedule(&ctx.resp_timeout_work, K_SECONDS(ANCS_RESPONSE_TIMEOUT_S));
	} else {
		ancs_finish_command(fail_status);
	}
}

/* ---------------------------------------------------------------------- */
/* GATT client callbacks (BLE host thread)                                 */
/* ---------------------------------------------------------------------- */

static void on_write_cmp(uint8_t conidx, uint8_t user_lid, uint16_t metainfo, uint16_t status)
{
	ARG_UNUSED(user_lid);

	switch (metainfo) {
	case ANCS_PROC_SUB_DS:
	case ANCS_PROC_SUB_NS_RESET:
	case ANCS_PROC_SUB_NS:
		on_sub_write_cmp(conidx, metainfo, status);
		break;
	case ANCS_PROC_CMD:
		on_cmd_write_cmp(conidx, status);
		break;
	case ANCS_PROC_UNSUB_NS:
	case ANCS_PROC_UNSUB_DS:
	default:
		break;
	}
}

static void on_att_val_evt(uint8_t conidx, uint8_t user_lid, uint16_t token, uint8_t evt_type,
			   bool complete, uint16_t hdl, co_buf_t *p_data)
{
	ARG_UNUSED(evt_type);
	ARG_UNUSED(complete);

	uint16_t len = co_buf_data_len(p_data);
	const uint8_t *data = co_buf_data(p_data);

	/* Defence-in-depth: only process notification content on an encrypted
	 * link. Subscription is already gated on encryption, but a peer could push
	 * Notification/Data Source events without a completed subscribe (or race
	 * an encryption drop), so drop plaintext content here too. */
	bool encrypted = halo_ble_is_encrypted();

	k_mutex_lock(&ancs_lock, K_FOREVER);
	bool is_ns = (encrypted && conidx == ctx.conidx && hdl == ctx.ns_val_hdl);
	bool is_ds = (encrypted && conidx == ctx.conidx && hdl == ctx.ds_val_hdl);
	k_mutex_unlock(&ancs_lock);

	if (is_ns) {
		if (len == ANCS_NS_EVENT_LEN) {
			struct halo_ancs_notification notif = {
				.event_id = data[0],
				.event_flags = data[1],
				.category_id = data[2],
				.category_count = data[3],
				.uid = co_letoh32(co_read32(&data[4])),
			};

			k_mutex_lock(&ancs_lock, K_FOREVER);
			if (ctx.cbs.notification) {
				ctx.cbs.notification(&notif, ctx.cbs.user_data);
			}
			k_mutex_unlock(&ancs_lock);
		} else {
			LOG_WRN("ANCS NS event with bad length %u", len);
		}
	} else if (is_ds) {
		ancs_ds_data(data, len);
	}

	/* Always confirm reception */
	alif_ble_mutex_lock(K_FOREVER);
	gatt_cli_att_event_cfm(conidx, user_lid, token);
	alif_ble_mutex_unlock();
}

static void on_svc_changed(uint8_t conidx, uint8_t user_lid, bool out_of_sync, uint16_t start_hdl,
			   uint16_t end_hdl)
{
	ARG_UNUSED(user_lid);

	bool rediscover = false;

	k_mutex_lock(&ancs_lock, K_FOREVER);

	if (conidx != ctx.conidx) {
		k_mutex_unlock(&ancs_lock);
		return;
	}

	/* Ignore changes that don't overlap the ANCS range, unless we don't
	 * know the range yet (ANCS may have just been published) or the peer
	 * is out of sync. */
	bool overlaps = out_of_sync || ctx.svc_start_hdl == GATT_INVALID_HDL ||
			!(end_hdl < ctx.svc_start_hdl || start_hdl > ctx.svc_end_hdl);

	if (overlaps && ctx.state != ANCS_STATE_IDLE) {
		LOG_INF("ANCS service changed [0x%04x-0x%04x], rediscovering", start_hdl, end_hdl);

		bool was_available = (ctx.state == ANCS_STATE_AVAILABLE);

		if (ctx.state == ANCS_STATE_DISCOVERING) {
			/* Let the in-flight discovery finish, then restart */
			ctx.rediscover = true;
		} else {
			ctx.state = ANCS_STATE_CONNECTED;
			ctx.disc_retries = 0;
			rediscover = true;
		}

		if (ctx.sub_state != ANCS_SUB_NONE) {
			ctx.sub_state = ctx.enabled ? ANCS_SUB_PENDING : ANCS_SUB_NONE;
			ctx.sub_retries = 0;
		}
		k_mutex_unlock(&ancs_lock);

		if (was_available) {
			notify_availability(false);
		}
		/* Abort any in-flight command - handles are stale */
		ancs_finish_command(-ENOTCONN);

		if (rediscover) {
			ancs_start_discovery();
		}
		return;
	}

	k_mutex_unlock(&ancs_lock);
}

static const gatt_cli_cb_t gatt_cli_cbs = {
	.cb_discover_cmp = on_discover_cmp,
	.cb_read_cmp = NULL,
	.cb_write_cmp = on_write_cmp,
	.cb_att_val_get = NULL,
	.cb_svc = on_svc,
	.cb_svc_info = NULL,
	.cb_inc_svc = NULL,
	.cb_char = NULL,
	.cb_desc = NULL,
	.cb_att_val = NULL,
	.cb_att_val_evt = on_att_val_evt,
	.cb_svc_changed = on_svc_changed,
};

/* ---------------------------------------------------------------------- */
/* Work handlers                                                           */
/* ---------------------------------------------------------------------- */

static void sub_retry_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	ancs_try_subscribe();
}

static void disc_retry_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	ancs_start_discovery();
}

static void resp_timeout_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	LOG_WRN("ANCS response timeout");
	ancs_finish_command(-ETIMEDOUT);
}

/* ---------------------------------------------------------------------- */
/* Connection lifecycle                                                    */
/* ---------------------------------------------------------------------- */

static int ancs_on_connected(uint8_t conidx)
{
	k_mutex_lock(&ancs_lock, K_FOREVER);
	ctx.conidx = conidx;
	ctx.state = ANCS_STATE_CONNECTED;
	ctx.sub_state = ctx.enabled ? ANCS_SUB_PENDING : ANCS_SUB_NONE;
	ctx.sub_retries = 0;
	ctx.disc_retries = 0;
	ctx.resp_len = 0;
	reset_handles();
	k_mutex_unlock(&ancs_lock);

	/* Discover ANCS. If the link is not yet encrypted iOS still answers
	 * primary service discovery; only the CCCD writes are gated. */
	ancs_start_discovery();
	return 0;
}

static int ancs_on_disconnected(uint8_t conidx)
{
	ARG_UNUSED(conidx);

	k_mutex_lock(&ancs_lock, K_FOREVER);
	bool was_available = (ctx.state == ANCS_STATE_AVAILABLE);

	ctx.conidx = GAP_INVALID_CONIDX;
	ctx.state = ANCS_STATE_IDLE;
	ctx.sub_state = ctx.enabled ? ANCS_SUB_PENDING : ANCS_SUB_NONE;
	ctx.sub_retries = 0;
	reset_handles();
	k_mutex_unlock(&ancs_lock);

	k_work_cancel_delayable(&ctx.sub_retry_work);
	k_work_cancel_delayable(&ctx.disc_retry_work);

	/* Fail any in-flight command */
	ancs_finish_command(-ENOTCONN);

	if (was_available) {
		notify_availability(false);
	}
	return 0;
}

/* ENCRYPTED fires once the link is actually encrypted (after fresh pairing or
 * bonded reconnection). Subscription is gated on encryption, so this is the
 * point at which a pending subscribe can finally proceed - reset the retry
 * budget and nudge it. */
static int ancs_ble_event_handler(const struct halo_ble_event_data *event, void *user_data)
{
	ARG_UNUSED(user_data);

	if (event->event == HALO_BLE_EVENT_ENCRYPTED) {
		k_mutex_lock(&ancs_lock, K_FOREVER);
		bool kick = (ctx.state == ANCS_STATE_AVAILABLE &&
			     ctx.sub_state == ANCS_SUB_PENDING);
		if (kick) {
			ctx.sub_retries = 0;
		}
		k_mutex_unlock(&ancs_lock);

		if (kick) {
			k_work_schedule(&ctx.sub_retry_work, K_MSEC(100));
		}
	}
	return 0;
}

static struct halo_ble_service ancs_service = {
	.name = "ANCS Client",
	.init = NULL,
	.deinit = NULL,
	.on_connected = ancs_on_connected,
	.on_disconnected = ancs_on_disconnected,
	.on_paired = NULL,
};

static struct halo_ble_callback ancs_ble_callback;

/* ---------------------------------------------------------------------- */
/* Public API                                                              */
/* ---------------------------------------------------------------------- */

int halo_ble_ancs_init(bool reset)
{
	int ret;

	if (ctx.initialized) {
		return 0;
	}

	k_work_init_delayable(&ctx.sub_retry_work, sub_retry_work_handler);
	k_work_init_delayable(&ctx.disc_retry_work, disc_retry_work_handler);
	k_work_init_delayable(&ctx.resp_timeout_work, resp_timeout_work_handler);

	ctx.conidx = GAP_INVALID_CONIDX;
	ctx.state = ANCS_STATE_IDLE;
	ctx.sub_state = ANCS_SUB_NONE;
	ctx.cmd_state = ANCS_CMD_IDLE;
	reset_handles();

	if (reset || !ctx.user_registered) {
		ctx.user_lid = GATT_INVALID_USER_LID;
		ret = gatt_user_cli_register(CFG_MAX_LE_MTU, 0, &gatt_cli_cbs, &ctx.user_lid);
		if (ret != GAP_ERR_NO_ERROR) {
			LOG_ERR("Failed to register GATT client user: 0x%04x", ret);
			return -EIO;
		}
		ctx.user_registered = true;
	}

	ret = halo_ble_service_register(&ancs_service);
	if (ret < 0) {
		LOG_ERR("Failed to register ANCS service hooks: %d", ret);
		return ret;
	}

	ret = halo_ble_register_callback(&ancs_ble_callback, ancs_ble_event_handler,
					 HALO_BLE_EVENT_MASK(HALO_BLE_EVENT_ENCRYPTED), NULL,
					 "ancs", 0);
	if (ret < 0) {
		LOG_WRN("Failed to register ANCS BLE event callback: %d", ret);
		/* Non-fatal: CCCD retry loop still converges without the nudge */
	}

	ctx.initialized = true;
	LOG_DBG("ANCS client initialized");
	return 0;
}

int halo_ble_ancs_deinit(void)
{
	if (!ctx.initialized) {
		return 0;
	}

	ctx.initialized = false;

	k_work_cancel_delayable(&ctx.sub_retry_work);
	k_work_cancel_delayable(&ctx.disc_retry_work);
	k_work_cancel_delayable(&ctx.resp_timeout_work);

	ancs_finish_command(-ENOTCONN);

	halo_ble_unregister_callback(&ancs_ble_callback);
	halo_ble_service_unregister(&ancs_service);

	k_mutex_lock(&ancs_lock, K_FOREVER);
	ctx.state = ANCS_STATE_IDLE;
	ctx.sub_state = ANCS_SUB_NONE;
	ctx.conidx = GAP_INVALID_CONIDX;
	reset_handles();
	k_mutex_unlock(&ancs_lock);

	return 0;
}

int halo_ble_ancs_set_callbacks(const struct halo_ancs_callbacks *cbs)
{
	k_mutex_lock(&ancs_lock, K_FOREVER);
	if (cbs) {
		ctx.cbs = *cbs;
	} else {
		memset(&ctx.cbs, 0, sizeof(ctx.cbs));
	}
	k_mutex_unlock(&ancs_lock);
	return 0;
}

int halo_ble_ancs_set_enabled(bool enable)
{
	if (!ctx.initialized) {
		return -ENODEV;
	}

	uint8_t conidx = GAP_INVALID_CONIDX;
	uint16_t ns_cccd = GATT_INVALID_HDL;
	uint16_t ds_cccd = GATT_INVALID_HDL;
	bool subscribe = false;

	k_mutex_lock(&ancs_lock, K_FOREVER);

	if (ctx.enabled == enable) {
		k_mutex_unlock(&ancs_lock);
		return 0;
	}
	ctx.enabled = enable;

	if (enable) {
		if (ctx.state == ANCS_STATE_AVAILABLE) {
			ctx.sub_state = ANCS_SUB_PENDING;
			ctx.sub_retries = 0;
			subscribe = true;
		} else {
			/* Subscription happens automatically after discovery */
			ctx.sub_state = ANCS_SUB_PENDING;
			ctx.sub_retries = 0;
		}
	} else {
		bool was_subscribed = (ctx.sub_state == ANCS_SUB_SUBSCRIBED ||
				       ctx.sub_state == ANCS_SUB_IN_PROGRESS);

		ctx.sub_state = ANCS_SUB_NONE;
		if (was_subscribed && ctx.state == ANCS_STATE_AVAILABLE) {
			conidx = ctx.conidx;
			ns_cccd = ctx.ns_cccd_hdl;
			ds_cccd = ctx.ds_cccd_hdl;
		}
	}

	k_mutex_unlock(&ancs_lock);

	if (subscribe) {
		ancs_try_subscribe();
	} else if (!enable) {
		k_work_cancel_delayable(&ctx.sub_retry_work);
		ancs_unsubscribe(conidx, ns_cccd, ds_cccd);
	}

	return 0;
}

bool halo_ble_ancs_is_available(void)
{
	return ctx.initialized && ctx.state == ANCS_STATE_AVAILABLE;
}

bool halo_ble_ancs_is_subscribed(void)
{
	return ctx.initialized && ctx.sub_state == ANCS_SUB_SUBSCRIBED;
}

/* Command parameters staged under lock before the Control Point write */
struct ancs_cmd_params {
	uint8_t cmd_id;
	uint32_t uid;
	uint8_t action_id;
	uint8_t attr_mask;
	uint8_t attr_count;
	const char *app_id;
};

/* Stage a command under lock; returns 0 and fills conidx/cp_hdl on success */
static int ancs_stage_command(const struct ancs_cmd_params *params, uint8_t *conidx,
			      uint16_t *cp_hdl)
{
	if (!ctx.initialized) {
		return -ENODEV;
	}

	k_mutex_lock(&ancs_lock, K_FOREVER);

	if (ctx.state != ANCS_STATE_AVAILABLE || ctx.conidx == GAP_INVALID_CONIDX) {
		k_mutex_unlock(&ancs_lock);
		return -ENOTCONN;
	}
	if (ctx.cmd_state != ANCS_CMD_IDLE) {
		k_mutex_unlock(&ancs_lock);
		return -EBUSY;
	}

	ctx.cmd_state = ANCS_CMD_WRITING;
	ctx.cmd_id = params->cmd_id;
	ctx.cmd_uid = params->uid;
	ctx.cmd_action_id = params->action_id;
	ctx.cmd_attr_mask = params->attr_mask;
	ctx.cmd_attr_count = params->attr_count;
	if (params->app_id) {
		strcpy(ctx.cmd_app_id, params->app_id);
	} else {
		ctx.cmd_app_id[0] = '\0';
	}
	ctx.resp_len = 0;
	*conidx = ctx.conidx;
	*cp_hdl = ctx.cp_val_hdl;

	k_mutex_unlock(&ancs_lock);
	return 0;
}

int halo_ble_ancs_get_notification_attrs(uint32_t uid, const struct halo_ancs_attr_request *req)
{
	uint8_t conidx;
	uint16_t cp_hdl;

	if (!req || attr_request_count(req) == 0) {
		return -EINVAL;
	}

	struct ancs_cmd_params params = {
		.cmd_id = ANCS_CMD_GET_NOTIF_ATTRS,
		.uid = uid,
		.attr_mask = req->attr_mask,
		.attr_count = attr_request_count(req),
	};

	int ret = ancs_stage_command(&params, &conidx, &cp_hdl);

	if (ret < 0) {
		return ret;
	}

	return ancs_send_command(conidx, cp_hdl, req);
}

int halo_ble_ancs_get_app_attrs(const char *app_id)
{
	uint8_t conidx;
	uint16_t cp_hdl;

	if (!app_id || app_id[0] == '\0' || strlen(app_id) > ANCS_APP_ID_MAX_LEN) {
		return -EINVAL;
	}

	struct ancs_cmd_params params = {
		.cmd_id = ANCS_CMD_GET_APP_ATTRS,
		.attr_count = 1,
		.app_id = app_id,
	};

	int ret = ancs_stage_command(&params, &conidx, &cp_hdl);

	if (ret < 0) {
		return ret;
	}

	return ancs_send_command(conidx, cp_hdl, NULL);
}

int halo_ble_ancs_perform_action(uint32_t uid, uint8_t action_id)
{
	uint8_t conidx;
	uint16_t cp_hdl;

	if (action_id != HALO_ANCS_ACTION_POSITIVE && action_id != HALO_ANCS_ACTION_NEGATIVE) {
		return -EINVAL;
	}

	struct ancs_cmd_params params = {
		.cmd_id = ANCS_CMD_PERFORM_ACTION,
		.uid = uid,
		.action_id = action_id,
	};

	int ret = ancs_stage_command(&params, &conidx, &cp_hdl);

	if (ret < 0) {
		return ret;
	}

	return ancs_send_command(conidx, cp_hdl, NULL);
}
