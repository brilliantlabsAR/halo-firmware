/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "alif_ble.h"
#include "gatt_db.h"
#include "gatt_srv.h"
#include "gap_le.h"
#include "co_endian.h"

#include "halo/ble_battery.h"
#include "halo/battery_manager.h"
#include "halo/ble_manager.h"
#include "halo/ble_service.h"

LOG_MODULE_REGISTER(halo_ble_battery, CONFIG_HALO_LOG_LEVEL);

/* Battery Service UUID: 0x180F */
#define BAS_UUID_16 0x180F

/* Battery Level Characteristic UUID: 0x2A19 */
#define BAS_BATTERY_LEVEL_UUID_16 0x2A19

/* Battery Power State Characteristic UUID: 0x2A1A */
#define BAS_BATTERY_POWER_STATE_UUID_16 0x2A1A

/* Battery Power State bit fields (BT SIG spec) */
/* Bit 0-1: Presence */
#define BPS_PRESENCE_UNKNOWN       (0 << 0) /* Unknown */
#define BPS_PRESENCE_NOT_SUPPORTED (1 << 0) /* Not Supported (No battery at all) */
#define BPS_PRESENCE_NOT_PRESENT   (2 << 0) /* Not Present (Battery could be added) */
#define BPS_PRESENCE_PRESENT       (3 << 0) /* Present */

/* Bit 2-3: Discharging */
#define BPS_DISCHARGING_UNKNOWN         (0 << 2) /* Unknown (No charge information) */
#define BPS_DISCHARGING_NOT_SUPPORTED   (1 << 2) /* Not Supported */
#define BPS_DISCHARGING_NOT_DISCHARGING (2 << 2) /* Not Discharging */
#define BPS_DISCHARGING_DISCHARGING     (3 << 2) /* Discharging */

/* Bit 4-5: Charging */
#define BPS_CHARGING_UNKNOWN (0 << 4) /* Unknown */
#define BPS_CHARGING_NOT_CHARGEABLE                                                                \
	(1 << 4)                           /* Not Chargeable (battery but no other power source) */
#define BPS_CHARGING_NOT_CHARGING (2 << 4) /* Not Charging (Chargeable) */
#define BPS_CHARGING_CHARGING     (3 << 4) /* Charging (Chargeable) */

/* Bit 6-7: Level */
#define BPS_LEVEL_UNKNOWN        (0 << 6) /* Unknown */
#define BPS_LEVEL_NOT_SUPPORTED  (1 << 6) /* Not Supported */
#define BPS_LEVEL_GOOD           (2 << 6) /* Good Level */
#define BPS_LEVEL_CRITICALLY_LOW (3 << 6) /* Critically Low Level (below 10%) */

#define BPS_INIT_MAGIC 0x42415473 /* 'BATS' */

/* GATT Attribute indices */
enum bas_att_idx {
	BAS_IDX_SERVICE = 0,
	BAS_IDX_BATTERY_LEVEL_CHAR,
	BAS_IDX_BATTERY_LEVEL_VAL,
	BAS_IDX_BATTERY_LEVEL_CCC,
	BAS_IDX_BATTERY_STATE_CHAR,
	BAS_IDX_BATTERY_STATE_VAL,
	BAS_IDX_BATTERY_STATE_CCC,
	BAS_IDX_NB,
};

/* BLE Battery Service context */
struct ble_battery_ctx {
	uint32_t initialized;
	uint16_t start_hdl;
	uint8_t user_lid;
	uint16_t level_ccc_cfg;  /* Battery Level CCC */
	uint16_t state_ccc_cfg;  /* Battery State CCC */
	uint8_t battery_level;   /* Current battery level (0-100%) */
	uint8_t battery_state;   /* Battery Power State (BT SIG format) */
	uint8_t conidx;          /* Connection index */
	struct k_sem notify_sem; /* Semaphore for notification flow control */
	struct k_mutex lock;
} bas_ctx __attribute__((noinit));

/* Helper macros for 16-bit UUID conversion to 128-bit array */
#define ATT_16_TO_128_ARRAY(uuid)                                                                  \
	{(uuid) & 0xFF, (uuid >> 8) & 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}

#define ATT_16_PRIMARY_SERVICE ATT_16_TO_128_ARRAY(GATT_DECL_PRIMARY_SERVICE)
#define ATT_16_CHARACTERISTIC  ATT_16_TO_128_ARRAY(GATT_DECL_CHARACTERISTIC)
#define ATT_16_CLIENT_CHAR_CFG ATT_16_TO_128_ARRAY(GATT_DESC_CLIENT_CHAR_CFG)

/* GATT attribute database for Battery Service */
static const gatt_att_desc_t bas_att_db[BAS_IDX_NB] = {
	/* Battery Service Declaration */
	[BAS_IDX_SERVICE] = {ATT_16_PRIMARY_SERVICE, ATT_UUID(16) | PROP(RD), 0},

	/* Battery Level Characteristic Declaration */
	[BAS_IDX_BATTERY_LEVEL_CHAR] = {ATT_16_CHARACTERISTIC, ATT_UUID(16) | PROP(RD), 0},

	/* Battery Level Characteristic Value */
	[BAS_IDX_BATTERY_LEVEL_VAL] =
		{
			{BAS_BATTERY_LEVEL_UUID_16 & 0xFF, BAS_BATTERY_LEVEL_UUID_16 >> 8, 0, 0, 0,
			 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
			ATT_UUID(16) | PROP(RD) | PROP(N),
			OPT(NO_OFFSET) | 1 /* 1 byte */
		},

	/* Battery Level CCC Descriptor */
	[BAS_IDX_BATTERY_LEVEL_CCC] =
		{
			ATT_16_CLIENT_CHAR_CFG, ATT_UUID(16) | PROP(RD) | PROP(WR),
			OPT(NO_OFFSET) | 2 /* 2 bytes */
		},

	/* Battery Power State Characteristic Declaration */
	[BAS_IDX_BATTERY_STATE_CHAR] = {ATT_16_CHARACTERISTIC, ATT_UUID(16) | PROP(RD), 0},

	/* Battery Power State Characteristic Value */
	[BAS_IDX_BATTERY_STATE_VAL] =
		{
			{BAS_BATTERY_POWER_STATE_UUID_16 & 0xFF,
			 BAS_BATTERY_POWER_STATE_UUID_16 >> 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			 0, 0},
			ATT_UUID(16) | PROP(RD) | PROP(N),
			OPT(NO_OFFSET) | 1 /* 1 byte: [6-7]=level, [4-5]=charging,
					      [2-3]=discharging, [0-1]=presence */
		},

	/* Battery Power State CCC Descriptor */
	[BAS_IDX_BATTERY_STATE_CCC] =
		{
			ATT_16_CLIENT_CHAR_CFG, ATT_UUID(16) | PROP(RD) | PROP(WR),
			OPT(NO_OFFSET) | 2 /* 2 bytes */
		},
};

/* Forward declarations */
static void notify_battery_state(uint8_t state);
static void battery_event_handler(enum halo_battery_event event, uint8_t level, void *user_data);
static int ble_battery_on_connected(uint8_t conidx);
static int ble_battery_on_disconnected(uint8_t conidx);

/* Battery manager callback */
HALO_BATTERY_CALLBACK_DEFINE(battery_cb, battery_event_handler, NULL, "BLE Battery Service");

/* GATT read callback */
static void on_att_read_get(uint8_t conidx, uint8_t user_lid, uint16_t token, uint16_t hdl,
			    uint16_t offset, uint16_t max_length)
{
	co_buf_t *p_buf = NULL;
	uint16_t status = GAP_ERR_NO_ERROR;
	uint16_t att_val_len = 0;
	uint8_t att_idx = hdl - bas_ctx.start_hdl;

	switch (att_idx) {
	case BAS_IDX_BATTERY_LEVEL_VAL:
		/* Read current battery level */
		att_val_len = 1;
		status = co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, att_val_len,
				      GATT_BUFFER_TAIL_LEN);
		if (status == CO_BUF_ERR_NO_ERROR && p_buf != NULL) {
			*co_buf_data(p_buf) = bas_ctx.battery_level;
		}
		break;

	case BAS_IDX_BATTERY_LEVEL_CCC:
		/* Read Battery Level CCC descriptor */
		att_val_len = 2;
		status = co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, att_val_len,
				      GATT_BUFFER_TAIL_LEN);
		if (status == CO_BUF_ERR_NO_ERROR && p_buf != NULL) {
			co_write16(co_buf_data(p_buf), co_htole16(bas_ctx.level_ccc_cfg));
		}
		break;

	case BAS_IDX_BATTERY_STATE_VAL:
		/* Read battery power state */
		att_val_len = 1;
		status = co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, att_val_len,
				      GATT_BUFFER_TAIL_LEN);
		if (status == CO_BUF_ERR_NO_ERROR && p_buf != NULL) {
			*co_buf_data(p_buf) = bas_ctx.battery_state;
		}
		break;

	case BAS_IDX_BATTERY_STATE_CCC:
		/* Read Battery State CCC descriptor */
		att_val_len = 2;
		status = co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, att_val_len,
				      GATT_BUFFER_TAIL_LEN);
		if (status == CO_BUF_ERR_NO_ERROR && p_buf != NULL) {
			co_write16(co_buf_data(p_buf), co_htole16(bas_ctx.state_ccc_cfg));
		}
		break;

	default:
		status = ATT_ERR_REQUEST_NOT_SUPPORTED;
		break;
	}

	/* Send confirmation */
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
	uint8_t att_idx = hdl - bas_ctx.start_hdl;

	switch (att_idx) {
	case BAS_IDX_BATTERY_LEVEL_CCC: {
		/* Write Battery Level CCC descriptor */
		if (co_buf_data_len(p_data) == 2) {
			uint16_t ccc_val = co_letoh16(co_read16(co_buf_data(p_data)));

			if (ccc_val == GATT_CCC_START_NTF) {
				bas_ctx.level_ccc_cfg = GATT_CCC_START_NTF;

				/* Send initial notification */
				halo_ble_battery_notify(bas_ctx.battery_level);
			} else {
				bas_ctx.level_ccc_cfg = GATT_CCC_STOP_NTFIND;
			}
		} else {
			status = ATT_ERR_INVALID_ATTRIBUTE_VAL_LEN;
		}
		break;
	}

	case BAS_IDX_BATTERY_STATE_CCC: {
		/* Write Battery State CCC descriptor */
		if (co_buf_data_len(p_data) == 2) {
			uint16_t ccc_val = co_letoh16(co_read16(co_buf_data(p_data)));

			if (ccc_val == GATT_CCC_START_NTF) {
				bas_ctx.state_ccc_cfg = GATT_CCC_START_NTF;
			} else {
				bas_ctx.state_ccc_cfg = GATT_CCC_STOP_NTFIND;
			}
		} else {
			status = ATT_ERR_INVALID_ATTRIBUTE_VAL_LEN;
		}
		break;
	}

	default:
		status = ATT_ERR_REQUEST_NOT_SUPPORTED;
		break;
	}

	/* Send confirmation first */
	gatt_srv_att_val_set_cfm(conidx, user_lid, token, status);

	/* After confirmation, send initial notifications if CCC was just enabled */
	if (status == GAP_ERR_NO_ERROR) {
		if (att_idx == BAS_IDX_BATTERY_LEVEL_CCC &&
		    bas_ctx.level_ccc_cfg == GATT_CCC_START_NTF) {
			/* Already sent via halo_ble_battery_notify() above */
		} else if (att_idx == BAS_IDX_BATTERY_STATE_CCC &&
			   bas_ctx.state_ccc_cfg == GATT_CCC_START_NTF) {
			/* Send initial battery state notification */
			notify_battery_state(bas_ctx.battery_state);
		}
	}
}

/* GATT event sent callback */
static void on_event_sent(uint8_t conidx, uint8_t user_lid, uint16_t metainfo, uint16_t status)
{
	if (status != GAP_ERR_NO_ERROR) {
		LOG_WRN("Battery notification failed: 0x%04X", status);
	}

	/* Release semaphore for flow control */
	k_sem_give(&bas_ctx.notify_sem);
}

/* GATT service callbacks */
static const gatt_srv_cb_t gatt_cbs = {
	.cb_att_event_get = NULL,
	.cb_att_info_get = NULL,
	.cb_att_read_get = on_att_read_get,
	.cb_att_val_set = on_att_val_set,
	.cb_event_sent = on_event_sent,
};

/* Send battery level notification */
int halo_ble_battery_notify(uint8_t level)
{
	/* Update cached level */
	bas_ctx.battery_level = level;

	if (bas_ctx.initialized != BPS_INIT_MAGIC) {
		return -ENODEV;
	}

	if (bas_ctx.conidx == GAP_INVALID_CONIDX) {
		return -ENOTCONN;
	}

	if (bas_ctx.level_ccc_cfg != GATT_CCC_START_NTF) {
		return 0;
	}

	/* Clamp level to 0-100 */
	if (level > 100) {
		level = 100;
	}

	/* Wait for previous notification to complete */
	if (k_sem_take(&bas_ctx.notify_sem, K_MSEC(1000)) != 0) {
		return -ETIMEDOUT;
	}

	/* Allocate buffer */
	co_buf_t *p_buf;
	uint16_t status = co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, 1, GATT_BUFFER_TAIL_LEN);
	if (status != CO_BUF_ERR_NO_ERROR) {
		LOG_ERR("Buffer allocation failed: %u", status);
		k_sem_give(&bas_ctx.notify_sem);
		return -ENOMEM;
	}

	/* Fill buffer with battery level */
	*co_buf_data(p_buf) = level;

	/* Send notification */
	alif_ble_mutex_lock(K_FOREVER);
	status = gatt_srv_event_send(bas_ctx.conidx, bas_ctx.user_lid, 0, GATT_NOTIFY,
				     bas_ctx.start_hdl + BAS_IDX_BATTERY_LEVEL_VAL, p_buf);
	alif_ble_mutex_unlock();

	co_buf_release(p_buf);

	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to send level notification: 0x%04X", status);
		k_sem_give(&bas_ctx.notify_sem);
		return -EIO;
	}

	return 0;
}

/* Send battery state notification */
static void notify_battery_state(uint8_t state)
{
	if (bas_ctx.initialized != BPS_INIT_MAGIC || bas_ctx.conidx == GAP_INVALID_CONIDX) {
		return;
	}

	if (bas_ctx.state_ccc_cfg != GATT_CCC_START_NTF) {
		return;
	}

	co_buf_t *p_buf;
	uint16_t ret = co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, 1, GATT_BUFFER_TAIL_LEN);
	if (ret != CO_BUF_ERR_NO_ERROR) {
		LOG_ERR("Failed to allocate buffer for state notification");
		return;
	}

	*co_buf_data(p_buf) = state;

	alif_ble_mutex_lock(K_FOREVER);
	ret = gatt_srv_event_send(bas_ctx.conidx, bas_ctx.user_lid, 0, GATT_NOTIFY,
				  bas_ctx.start_hdl + BAS_IDX_BATTERY_STATE_VAL, p_buf);
	alif_ble_mutex_unlock();

	co_buf_release(p_buf);

	if (ret != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to send state notification: 0x%04X", ret);
	}
}

/* Battery manager event handler */
static void battery_event_handler(enum halo_battery_event event, uint8_t level, void *user_data)
{

	switch (event) {
	case HALO_BATTERY_EVENT_LEVEL_CHANGED:
		halo_ble_battery_notify(level);
		break;

	case HALO_BATTERY_EVENT_CHARGING_STARTED:
		k_mutex_lock(&bas_ctx.lock, K_FOREVER);
		/* Battery present, not discharging, charging, good level */
		bas_ctx.battery_state = BPS_PRESENCE_PRESENT | BPS_DISCHARGING_NOT_DISCHARGING |
					BPS_CHARGING_CHARGING | BPS_LEVEL_GOOD;
		k_mutex_unlock(&bas_ctx.lock);
		notify_battery_state(bas_ctx.battery_state);
		break;

	case HALO_BATTERY_EVENT_CHARGING_STOPPED:
		k_mutex_lock(&bas_ctx.lock, K_FOREVER);
		/* Battery present, discharging, not charging, good level */
		bas_ctx.battery_state = BPS_PRESENCE_PRESENT | BPS_DISCHARGING_DISCHARGING |
					BPS_CHARGING_NOT_CHARGING | BPS_LEVEL_GOOD;
		k_mutex_unlock(&bas_ctx.lock);
		notify_battery_state(bas_ctx.battery_state);
		break;

	case HALO_BATTERY_EVENT_LOW_BATTERY:
		LOG_WRN("Low battery: %u%%", level);
		halo_ble_battery_notify(level);
		break;

	case HALO_BATTERY_EVENT_CRITICAL_BATTERY:
		LOG_ERR("Critical battery: %u%%", level);
		halo_ble_battery_notify(level);
		break;

	default:
		break;
	}
}

/* BLE connection callback */
static int ble_battery_on_connected(uint8_t conidx)
{
	k_mutex_lock(&bas_ctx.lock, K_FOREVER);
	bas_ctx.conidx = conidx;
	k_mutex_unlock(&bas_ctx.lock);

	return 0;
}

/* BLE disconnection callback */
static int ble_battery_on_disconnected(uint8_t conidx)
{
	k_mutex_lock(&bas_ctx.lock, K_FOREVER);
	bas_ctx.conidx = GAP_INVALID_CONIDX;
	bas_ctx.level_ccc_cfg = GATT_CCC_STOP_NTFIND;
	bas_ctx.state_ccc_cfg = GATT_CCC_STOP_NTFIND;
	k_mutex_unlock(&bas_ctx.lock);

	/* Release semaphore in case notification was pending */
	k_sem_give(&bas_ctx.notify_sem);

	return 0;
}

/* BLE service structure */
static struct halo_ble_service battery_service = {
	.name = "Battery Service",
	.init = NULL, /* Initialized separately */
	.deinit = NULL,
	.on_connected = ble_battery_on_connected,
	.on_disconnected = ble_battery_on_disconnected,
	.on_paired = NULL,
};

/* Public API implementations */

int halo_ble_battery_init(bool reset)
{
	int ret;

	/* Initialize synchronization primitives */
	k_sem_init(&bas_ctx.notify_sem, 1, 1);
	k_mutex_init(&bas_ctx.lock);

	if (bas_ctx.initialized == BPS_INIT_MAGIC) {
		return 0;
	}

	/* Register GATT user service */
	if (reset) {
		bas_ctx.user_lid = GATT_INVALID_USER_LID;
		bas_ctx.start_hdl = GATT_INVALID_HDL;
		bas_ctx.level_ccc_cfg = 0;
		bas_ctx.state_ccc_cfg = 0;
		bas_ctx.battery_level = 100;
		/* Initial state: Battery present, not charging, discharging, good level */
		bas_ctx.battery_state = BPS_PRESENCE_PRESENT | BPS_DISCHARGING_DISCHARGING |
					BPS_CHARGING_NOT_CHARGING | BPS_LEVEL_GOOD,
		bas_ctx.conidx = GAP_INVALID_CONIDX;

		ret = gatt_user_srv_register(CFG_MAX_LE_MTU, 0, &gatt_cbs, &bas_ctx.user_lid);
		if (ret != GAP_ERR_NO_ERROR) {
			LOG_ERR("Failed to register GATT user service: %d", ret);
			return -EIO;
		}

		/* Add Battery Service to GATT database */
		uint8_t uuid[2] = {BAS_UUID_16 & 0xFF, BAS_UUID_16 >> 8};
		ret = gatt_db_svc_add(bas_ctx.user_lid, SVC_UUID(16), uuid, BAS_IDX_NB, NULL,
				      bas_att_db, BAS_IDX_NB, &bas_ctx.start_hdl);
		if (ret != GAP_ERR_NO_ERROR) {
			LOG_ERR("Failed to add Battery Service: %d", ret);
			return -EIO;
		}
	}

	/* Get initial battery level and state */
	int level = halo_battery_get_level();
	if (level >= 0) {
		bas_ctx.battery_level = (uint8_t)level;
	}

	bool charging = halo_battery_is_charging();
	if (charging) {
		/* Battery present, not discharging, charging, good level */
		bas_ctx.battery_state = BPS_PRESENCE_PRESENT | BPS_DISCHARGING_NOT_DISCHARGING |
					BPS_CHARGING_CHARGING | BPS_LEVEL_GOOD;
	} else {
		/* Battery present, discharging, not charging, good level */
		bas_ctx.battery_state = BPS_PRESENCE_PRESENT | BPS_DISCHARGING_DISCHARGING |
					BPS_CHARGING_NOT_CHARGING | BPS_LEVEL_GOOD;
	}

	/* Register battery manager callback */
	ret = halo_battery_register_callback(&battery_cb);
	if (ret < 0) {
		LOG_ERR("Failed to register battery callback: %d", ret);
		return ret;
	}

	/* Register BLE service */
	ret = halo_ble_service_register(&battery_service);
	if (ret < 0) {
		LOG_ERR("Failed to register BLE service: %d", ret);
		halo_battery_unregister_callback(&battery_cb);
		return ret;
	}

	bas_ctx.initialized = BPS_INIT_MAGIC;

	LOG_DBG("BLE Battery Service initialized successfully");
	return 0;
}

int halo_ble_battery_deinit(void)
{
	if (bas_ctx.initialized != BPS_INIT_MAGIC) {
		return 0;
	}

	/* Unregister callbacks */
	halo_battery_unregister_callback(&battery_cb);
	halo_ble_service_unregister(&battery_service);

	/* TODO: Remove service from GATT database if API available */

	bas_ctx.initialized = 0;
	return 0;
}

bool halo_ble_battery_is_notify_enabled(void)
{
	return bas_ctx.initialized == BPS_INIT_MAGIC && bas_ctx.conidx != GAP_INVALID_CONIDX &&
	       (bas_ctx.level_ccc_cfg == GATT_CCC_START_NTF ||
		bas_ctx.state_ccc_cfg == GATT_CCC_START_NTF);
}
