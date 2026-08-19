/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/slist.h>

#include "halo/lua_service.h"

LOG_MODULE_REGISTER(halo_lua_svc, CONFIG_HALO_LOG_LEVEL);

/* Linked list of registered services */
static sys_slist_t service_list = SYS_SLIST_STATIC_INIT(&service_list);
static struct k_mutex service_lock;
static bool initialized = false;

/* Event name strings for debugging */
static const char *event_names[] = {
	[HALO_LUA_EVENT_INIT] = "INIT",           [HALO_LUA_EVENT_DEINIT] = "DEINIT",
	[HALO_LUA_EVENT_INTERRUPT] = "INTERRUPT", [HALO_LUA_EVENT_SUSPEND] = "SUSPEND",
	[HALO_LUA_EVENT_RESUME] = "RESUME",
};

const char *halo_lua_event_name(halo_lua_event_t event)
{
	if (event < ARRAY_SIZE(event_names)) {
		return event_names[event];
	}
	return "UNKNOWN";
}

static void lua_service_init_once(void)
{
	if (!initialized) {
		k_mutex_init(&service_lock);
		initialized = true;
	}
}

int halo_lua_service_register(struct halo_lua_service *service)
{
	if (!service || !service->callback) {
		return -EINVAL;
	}

	lua_service_init_once();

	k_mutex_lock(&service_lock, K_FOREVER);

	/* Check if already registered */
	sys_snode_t *node;
	SYS_SLIST_FOR_EACH_NODE(&service_list, node) {
		struct halo_lua_service *s = CONTAINER_OF(node, struct halo_lua_service, node);
		if (s == service) {
			k_mutex_unlock(&service_lock);
			return -EALREADY;
		}
	}

	/* Initialize power management state to active by default
	 * Services that want to start in power save mode should set is_active=false
	 * in their INIT event handler
	 */
	service->is_active = true;
	service->needs_resume = false;

	/* Add to list */
	sys_slist_append(&service_list, &service->node);
	k_mutex_unlock(&service_lock);
	return 0;
}

int halo_lua_service_unregister(struct halo_lua_service *service)
{
	if (!service) {
		return -EINVAL;
	}

	if (!initialized) {
		return -ENOENT;
	}

	k_mutex_lock(&service_lock, K_FOREVER);

	bool found = sys_slist_find_and_remove(&service_list, &service->node);

	k_mutex_unlock(&service_lock);

	if (found) {
		return 0;
	}

	return -ENOENT;
}

int halo_lua_service_notify(halo_lua_event_t event)
{
	if (!initialized) {
		return 0; /* No services registered yet */
	}

	k_mutex_lock(&service_lock, K_FOREVER);

	int ret = 0;
	sys_snode_t *node;

	SYS_SLIST_FOR_EACH_NODE(&service_list, node) {
		struct halo_lua_service *service =
			CONTAINER_OF(node, struct halo_lua_service, node);

		/* Special handling for SUSPEND/RESUME based on service state */
		if (event == HALO_LUA_EVENT_SUSPEND) {
			/* Always-on services receive SUSPEND for hardware teardown but
			 * never need a matching RESUME (they manage their own state). */
			if (service->always_on) {
				service->callback(event, service->user_data);
				service->needs_resume = false;
				continue;
			}

			/* Only notify if service is active */
			if (!service->is_active) {
				service->needs_resume = false;
				continue;
			}
			int result = service->callback(event, service->user_data);
			if (result < 0) {
				LOG_ERR("Service '%s' failed to handle event %s: %d", service->name,
					halo_lua_event_name(event), result);
				ret = result;
				service->needs_resume = false;
			} else {
				/* Result 0 = needs resume, 1 = skip resume */
				service->needs_resume = (result == 0);
			}
		} else if (event == HALO_LUA_EVENT_RESUME) {
			/* Only notify if service needs resume */
			if (!service->needs_resume) {
				continue;
			}
			int result = service->callback(event, service->user_data);
			if (result < 0) {
				LOG_ERR("Service '%s' failed to handle event %s: %d", service->name,
					halo_lua_event_name(event), result);
				ret = result;
			}
			service->needs_resume = false;
		} else {
			/* For other events, notify all services */
			int result = service->callback(event, service->user_data);
			if (result < 0) {
				LOG_ERR("Service '%s' failed to handle event %s: %d", service->name,
					halo_lua_event_name(event), result);
				ret = result; /* Record first error */
			}
		}
	}

	k_mutex_unlock(&service_lock);

	return ret;
}

int halo_lua_service_power_save(struct halo_lua_service *service, bool enable)
{
	if (!service || !service->callback) {
		return -EINVAL;
	}

	if (!initialized) {
		return -ENOENT;
	}

	k_mutex_lock(&service_lock, K_FOREVER);

	int ret = 0;

	if (enable) {
		/* Enter power save mode (suspend) */
		if (!service->is_active) {
			/* Already in power save, nothing to do */
			k_mutex_unlock(&service_lock);
			return 0;
		}

		ret = service->callback(HALO_LUA_EVENT_SUSPEND, service->user_data);

		if (ret < 0) {
			LOG_ERR("Service '%s' failed to suspend: %d", service->name, ret);
		} else {
			/* Result 0 = needs resume, 1 = skip resume */
			service->needs_resume = (ret == 0);
			service->is_active = false;
			ret = 0; /* Success */
		}
	} else {
		/* Exit power save mode (resume) */
		if (service->is_active) {
			/* Already active, nothing to do */
			k_mutex_unlock(&service_lock);
			return 0;
		}

		/* User-initiated power save exit: always call RESUME callback
		 * This is different from system PM resume which respects needs_resume flag
		 */
		ret = service->callback(HALO_LUA_EVENT_RESUME, service->user_data);

		if (ret < 0) {
			LOG_ERR("Service '%s' failed to resume: %d", service->name, ret);
			/* Don't set is_active = true on failure */
		} else {
			service->needs_resume = false;
			service->is_active = true;
		}
	}

	k_mutex_unlock(&service_lock);

	return ret;
}
