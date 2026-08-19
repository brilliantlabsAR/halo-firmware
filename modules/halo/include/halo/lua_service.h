/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_LUA_SERVICE_H_
#define HALO_LUA_SERVICE_H_

#include <zephyr/kernel.h>
#include <zephyr/sys/slist.h>

/**
 * @brief Lua service lifecycle events
 */
typedef enum {
	HALO_LUA_EVENT_INIT,      /**< Lua VM initialized */
	HALO_LUA_EVENT_DEINIT,    /**< Lua VM closing */
	HALO_LUA_EVENT_INTERRUPT, /**< Ctrl+C - Interrupt execution */
	HALO_LUA_EVENT_SUSPEND,   /**< System suspend */
	HALO_LUA_EVENT_RESUME,    /**< System resume */
} halo_lua_event_t;

/**
 * @brief Lua service callback function
 *
 * @param event The lifecycle event
 * @param user_data User-provided data
 * @return 0 on success, negative errno on error
 */
typedef int (*halo_lua_service_callback_t)(halo_lua_event_t event, void *user_data);

/**
 * @brief Lua service structure
 *
 * Services can register callbacks to handle lifecycle events.
 * This allows libraries (camera, audio, IMU, etc.) to:
 * - Initialize resources when VM starts
 * - Clean up threads/resources when VM stops
 * - Handle interrupt signals (Ctrl+C)
 * - Prepare for suspend/resume
 */
struct halo_lua_service {
	const char *name;
	halo_lua_service_callback_t callback;
	void *user_data;
	sys_snode_t node;

	/* Power management state - managed by framework */
	bool is_active;       /**< Service currently active (not in power save) */
	bool needs_resume;    /**< Service needs resume after suspend */
	bool always_on;       /**< Service is always-on, skip suspend/resume */
};

/**
 * @brief Define a Lua service statically
 *
 * Example:
 *   HALO_LUA_SERVICE_DEFINE(camera_service, camera_lifecycle_cb, NULL, false);
 */
#define HALO_LUA_SERVICE_DEFINE(_name, _callback, _user_data, _always_on)                          \
	static struct halo_lua_service _name = {                                                   \
		.name = STRINGIFY(_name),                                                          \
		.callback = _callback,                                                             \
		.user_data = _user_data,                                                           \
		.is_active = false,                                                                \
		.needs_resume = false,                                                             \
		.always_on = _always_on,                                                           \
	}

/**
 * @brief Register a Lua service
 *
 * @param service Pointer to service structure
 * @return 0 on success, negative errno on error
 */
int halo_lua_service_register(struct halo_lua_service *service);

/**
 * @brief Unregister a Lua service
 *
 * @param service Pointer to service structure
 * @return 0 on success, negative errno on error
 */
int halo_lua_service_unregister(struct halo_lua_service *service);

/**
 * @brief Notify all registered services of an event
 *
 * Called internally by lua_runtime when lifecycle events occur.
 *
 * @param event The event to broadcast
 * @return 0 on success, negative errno if any service fails
 */
int halo_lua_service_notify(halo_lua_event_t event);

/**
 * @brief Control power save mode for a specific service
 *
 * This provides a unified interface for services to enter/exit power save mode.
 * When entering power save (enable=true), the service's SUSPEND event handler is called.
 * When exiting power save (enable=false), the service's RESUME event handler is called
 * only if it was active before suspend.
 *
 * @param service Pointer to service structure
 * @param enable true to enter power save (suspend), false to exit (resume)
 * @return 0 on success, negative errno on error
 */
int halo_lua_service_power_save(struct halo_lua_service *service, bool enable);

/**
 * @brief Get event name string (for debugging)
 *
 * @param event The event
 * @return Event name string
 */
const char *halo_lua_event_name(halo_lua_event_t event);

#endif /* HALO_LUA_SERVICE_H_ */
