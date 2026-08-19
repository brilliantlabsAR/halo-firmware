/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/dlist.h>
#include <string.h>

#include "halo/ble_manager.h"
#include "halo/ble_connection.h"
#include "halo/ble_security.h"
#include "halo/pm_manager.h"

#ifdef CONFIG_HALO_BLE_BATTERY_SERVICE
#include "halo/ble_battery.h"
#endif

#ifdef CONFIG_HALO_BLE_LUA_SERVICE
#include "halo/ble_lua.h"
#endif


#ifdef CONFIG_HALO_BLE_AUDIO_SERVICE
#include "halo/ble_audio.h"
#endif

#ifdef CONFIG_HALO_BLE_OTA_SERVICE
#include "halo/ble_ota.h"
#endif

#ifdef CONFIG_HALO_BLE_ANCS_CLIENT
#include "halo/ble_ancs.h"
#endif

LOG_MODULE_REGISTER(halo_ble, CONFIG_HALO_LOG_LEVEL);

/* BLE manager context */
static struct {
	bool initialized;
	struct k_mutex lock;
	sys_dlist_t callbacks; /* List of registered callbacks */
} ble_ctx;

/**
 * @brief Dispatch event to all registered callbacks
 */
static void ble_dispatch_event(const struct halo_ble_event_data *event)
{
	sys_dnode_t *node;
	struct halo_ble_callback *cb;
	uint32_t event_bit = HALO_BLE_EVENT_MASK(event->event);
	int ret;

	k_mutex_lock(&ble_ctx.lock, K_FOREVER);

	/* Call callbacks in priority order (highest first) */
	SYS_DLIST_FOR_EACH_NODE(&ble_ctx.callbacks, node) {
		cb = CONTAINER_OF(node, struct halo_ble_callback, node);

		/* Check if callback is interested in this event */
		if ((cb->event_mask & event_bit) == 0) {
			continue;
		}

		ret = cb->callback(event, cb->user_data);
		if (ret < 0) {
			LOG_WRN("Callback %s returned error %d", cb->name, ret);
		}
	}

	k_mutex_unlock(&ble_ctx.lock);
}

/**
 * @brief Internal function to notify event (called by sub-modules)
 */
void halo_ble_notify_event(const struct halo_ble_event_data *event)
{
	ble_dispatch_event(event);
}

/**
 * @brief PM callback handler for BLE power management
 *
 * In light sleep (light-sleep) mode, BLE is kept active to maintain connection.
 * In deep sleep mode, BLE would be suspended (not implemented yet).
 */
static int ble_pm_callback_handler(halo_pm_event_t event, halo_pm_sleep_mode_t mode,
				   void *user_data)
{
	ARG_UNUSED(user_data);
	int ret = 0;

	if (mode != HALO_PM_SLEEP_DEEP) {
		/* In light/standby sleep, BLE remains active */
		return 1; /* Skip - no resume needed */
	}

	switch (event) {
	case HALO_PM_EVENT_SUSPEND:
		/* Deep sleep is a shutdown: the SoC powers off and reboots on
		 * wake, so a service that fails to deinit (e.g. audio still
		 * streaming) must not veto the power-off. Log and continue —
		 * aborting here left the device on with the display dark. */
#ifdef CONFIG_HALO_BLE_BATTERY_SERVICE
		ret = halo_ble_battery_deinit();
		if (ret < 0) {
			LOG_WRN("BLE battery service deinit failed: %d (continuing)", ret);
		}
#endif
#ifdef CONFIG_HALO_BLE_LUA_SERVICE
		ret = halo_ble_lua_deinit();
		if (ret < 0) {
			LOG_WRN("BLE Lua service deinit failed: %d (continuing)", ret);
		}
#endif
#ifdef CONFIG_HALO_BLE_AUDIO_SERVICE
		ret = halo_ble_audio_deinit();
		if (ret < 0) {
			LOG_WRN("BLE audio service deinit failed: %d (continuing)", ret);
		}
#endif
#ifdef CONFIG_HALO_BLE_OTA_SERVICE
		ret = halo_ble_ota_deinit();
		if (ret < 0) {
			LOG_WRN("BLE OTA service deinit failed: %d (continuing)", ret);
		}
#endif
#ifdef CONFIG_HALO_BLE_ANCS_CLIENT
		ret = halo_ble_ancs_deinit();
		if (ret < 0) {
			LOG_WRN("ANCS client deinit failed: %d (continuing)", ret);
		}
#endif
		ret = halo_ble_conn_deinit(mode == HALO_PM_SLEEP_DEEP);
		if (ret < 0) {
			LOG_WRN("BLE connection manager deinit failed: %d (continuing)", ret);
		}
		break;
	case HALO_PM_EVENT_RESUME:
		/* code */
		break;

	default:
		break;
	}

	return 0; /* Skip by default */
}

/* PM callback structure with priority 20 (high priority for connectivity) */
static struct halo_pm_callback ble_pm_callback;

int halo_ble_init(const char *device_name)
{
	int ret;
	bool reset = false;

	if (ble_ctx.initialized) {
		return 0;
	}

	k_mutex_init(&ble_ctx.lock);
	sys_dlist_init(&ble_ctx.callbacks);

	/* Initialize sub-modules */
	ret = halo_ble_conn_init(device_name);
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("Failed to initialize connection manager: %d", ret);
		return ret;
	}

	reset = (ret == 0);

	ret = halo_ble_sec_init();
	if (ret < 0) {
		LOG_ERR("Failed to initialize security manager: %d", ret);
		return ret;
	}

#ifdef CONFIG_HALO_BLE_BATTERY_SERVICE
	/* Initialize Battery Service */
	ret = halo_ble_battery_init(reset);
	if (ret < 0) {
		LOG_WRN("Failed to initialize battery service: %d (continuing)", ret);
		/* Non-fatal, continue initialization */
	}
#endif

#ifdef CONFIG_HALO_BLE_LUA_SERVICE
	/* Initialize Lua Service */
	ret = halo_ble_lua_init(reset);
	if (ret < 0) {
		LOG_WRN("Failed to initialize Lua service: %d (continuing)", ret);
		/* Non-fatal, continue initialization */
	}
#endif

#ifdef CONFIG_HALO_BLE_AUDIO_SERVICE
	/* Initialize LE Audio Service */
	ret = halo_ble_audio_init(reset);
	if (ret < 0) {
		LOG_WRN("Failed to initialize LE Audio service: %d (continuing)", ret);
		/* Non-fatal, continue initialization */
	}
#endif

#ifdef CONFIG_HALO_BLE_OTA_SERVICE
	/* Initialize OTA Service */
	ret = halo_ble_ota_init(reset);
	if (ret < 0) {
		LOG_WRN("Failed to initialize OTA service: %d (continuing)", ret);
		/* Non-fatal, continue initialization */
	}
#endif

	/* Register PM callback with high priority for connectivity */
	ret = halo_pm_register_callback(&ble_pm_callback, ble_pm_callback_handler, NULL,
					"ble_manager", 20); /* High priority for connectivity */
	if (ret != 0) {
		LOG_ERR("Failed to register BLE PM callback: %d", ret);
		/* Non-fatal, continue */
	}

	ble_ctx.initialized = true;

#ifdef CONFIG_HALO_BLE_ANCS_CLIENT
	/* Initialize ANCS client (after the manager is marked initialized so
	 * the client can register its BLE event callback) */
	ret = halo_ble_ancs_init(reset);
	if (ret < 0) {
		LOG_WRN("Failed to initialize ANCS client: %d (continuing)", ret);
		/* Non-fatal, continue initialization */
	}
#endif

	/* Start BLE advertising */
	static struct halo_ble_adv_params adv_params = {
		.interval_min = 40,  /* 0.625ms units, 40 = 25ms */
		.interval_max = 200, /* 0.625ms units, 200 = 125ms */
		.duration = 0,       /* Advertise indefinitely */
		.channel_map = 0x07, /* All channels */
	};

	if (reset) {
		ret = halo_ble_adv_start(&adv_params);
		if (ret < 0) {
			LOG_ERR("Failed to start advertising: %d", ret);
			return ret;
		}
	}
	LOG_INF("BLE manager initialized");
	return 0;
}

int halo_ble_adv_start(const struct halo_ble_adv_params *params)
{
	if (!ble_ctx.initialized) {
		return -ENODEV;
	}

	return halo_ble_conn_adv_start(params);
}

int halo_ble_adv_stop(void)
{
	if (!ble_ctx.initialized) {
		return -ENODEV;
	}

	return halo_ble_conn_adv_stop();
}

int halo_ble_disconnect(void)
{
	if (!ble_ctx.initialized) {
		return -ENODEV;
	}

	return halo_ble_conn_disconnect();
}

int halo_ble_register_callback(struct halo_ble_callback *cb, halo_ble_event_cb_t callback,
			       uint32_t event_mask, void *user_data, const char *name, int priority)
{
	if (!cb || !callback) {
		return -EINVAL;
	}

	if (!ble_ctx.initialized) {
		return -ENODEV;
	}

	cb->callback = callback;
	cb->event_mask = event_mask;
	cb->user_data = user_data;
	cb->name = name ? name : "unnamed";
	cb->priority = priority;

	k_mutex_lock(&ble_ctx.lock, K_FOREVER);

	/* Insert callback in priority order (highest priority first) */
	sys_dnode_t *node;
	struct halo_ble_callback *item;
	bool inserted = false;

	SYS_DLIST_FOR_EACH_NODE(&ble_ctx.callbacks, node) {
		item = CONTAINER_OF(node, struct halo_ble_callback, node);
		if (priority > item->priority) {
			sys_dlist_insert(node->prev, &cb->node);
			inserted = true;
			break;
		}
	}

	if (!inserted) {
		sys_dlist_append(&ble_ctx.callbacks, &cb->node);
	}

	k_mutex_unlock(&ble_ctx.lock);

	return 0;
}

int halo_ble_unregister_callback(struct halo_ble_callback *cb)
{
	if (!cb) {
		return -EINVAL;
	}

	if (!ble_ctx.initialized) {
		return -ENODEV;
	}

	k_mutex_lock(&ble_ctx.lock, K_FOREVER);
	sys_dlist_remove(&cb->node);
	k_mutex_unlock(&ble_ctx.lock);

	return 0;
}

bool halo_ble_is_connected(void)
{
	if (!ble_ctx.initialized) {
		return false;
	}
	return halo_ble_conn_is_connected();
}

bool halo_ble_is_paired(void)
{
	if (!ble_ctx.initialized) {
		return false;
	}
	return halo_ble_sec_is_paired();
}

bool halo_ble_is_encrypted(void)
{
	if (!ble_ctx.initialized) {
		return false;
	}
	return halo_ble_sec_is_encrypted();
}

bool halo_ble_is_advertising(void)
{
	if (!ble_ctx.initialized) {
		return false;
	}
	return halo_ble_conn_is_advertising();
}

uint8_t halo_ble_get_conidx(void)
{
	if (!ble_ctx.initialized) {
		return 0xFF; /* GAP_INVALID_CONIDX */
	}
	return halo_ble_conn_get_conidx();
}

uint16_t halo_ble_get_mtu(void)
{
	if (!ble_ctx.initialized) {
		return 0;
	}
	return halo_ble_conn_get_mtu();
}

int halo_ble_get_address(uint8_t addr[6])
{
	if (!addr) {
		return -EINVAL;
	}

	if (!ble_ctx.initialized) {
		return -ENODEV;
	}

	return halo_ble_conn_get_address(addr);
}

int halo_ble_update_conn_params(const struct halo_ble_conn_params *params)
{
	if (!params) {
		return -EINVAL;
	}

	if (!ble_ctx.initialized) {
		return -ENODEV;
	}

	return halo_ble_conn_update_params(params);
}