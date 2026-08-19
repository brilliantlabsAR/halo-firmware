/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_BATTERY_MANAGER_H
#define HALO_BATTERY_MANAGER_H

#include <zephyr/kernel.h>
#include <zephyr/sys/dlist.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Battery event types
 */
enum halo_battery_event {
	HALO_BATTERY_EVENT_LEVEL_CHANGED,    /**< Battery level changed */
	HALO_BATTERY_EVENT_CHARGING_STARTED, /**< Charging started */
	HALO_BATTERY_EVENT_CHARGING_STOPPED, /**< Charging stopped */
	HALO_BATTERY_EVENT_LOW_BATTERY,      /**< Low battery warning */
	HALO_BATTERY_EVENT_CRITICAL_BATTERY, /**< Critical battery level */
};

/**
 * @brief Battery event callback function type
 * 
 * @param event Event type
 * @param level Current battery level (0-100%)
 * @param user_data User data provided during registration
 */
typedef void (*halo_battery_event_cb_t)(enum halo_battery_event event, 
                                         uint8_t level, void *user_data);

/**
 * @brief Battery callback structure
 * 
 * Used to register callbacks for battery events.
 * Should be statically allocated and remain valid for the lifetime of registration.
 */
struct battery_callback {
	sys_dnode_t node;                    /**< List node (internal use) */
	halo_battery_event_cb_t callback;    /**< Callback function */
	void *user_data;                     /**< User data passed to callback */
	const char *name;                    /**< Callback name for debugging */
};

/**
 * @brief Define and initialize a battery callback statically
 * 
 * @param _name Variable name for the callback structure
 * @param _cb Callback function
 * @param _user_data User data pointer
 * @param _desc Description string for debugging
 */
#define HALO_BATTERY_CALLBACK_DEFINE(_name, _cb, _user_data, _desc) \
	static struct battery_callback _name = { \
		.callback = _cb, \
		.user_data = _user_data, \
		.name = _desc, \
	}

/**
 * @brief Battery information structure
 */
struct halo_battery_info {
	uint8_t level;           /**< Battery level (0-100%) */
	uint16_t voltage_mv;     /**< Battery voltage in millivolts */
	bool is_charging;        /**< Charging status */
	int16_t temperature;     /**< Battery temperature in 0.1°C (if available) */
};

/**
 * @brief Initialize battery manager
 * 
 * Initializes battery monitoring hardware and starts periodic updates.
 * 
 * @return 0 on success, negative error code on failure
 */
int halo_battery_init(void);

/**
 * @brief Deinitialize battery manager
 * 
 * Stops battery monitoring and releases resources.
 * 
 * @return 0 on success, negative error code on failure
 */
int halo_battery_deinit(void);

/**
 * @brief Get current battery level
 * 
 * Returns the most recent battery level reading.
 * 
 * @return Battery level (0-100%), or negative error code
 */
int halo_battery_get_level(void);

/**
 * @brief Get battery voltage
 * 
 * Returns the most recent battery voltage reading.
 * 
 * @return Battery voltage in millivolts, or negative error code
 */
int halo_battery_get_voltage(void);

/**
 * @brief Check if battery is charging
 * 
 * @return true if charging, false otherwise
 */
bool halo_battery_is_charging(void);

/**
 * @brief Get complete battery information
 * 
 * @param info Pointer to structure to fill with battery information
 * @return 0 on success, negative error code on failure
 */
int halo_battery_get_info(struct halo_battery_info *info);

/**
 * @brief Register battery event callback
 * 
 * Registers a callback to be notified of battery events.
 * Multiple callbacks can be registered. The callback structure must remain
 * valid for the lifetime of the registration.
 * 
 * @param cb Pointer to callback structure (must be statically allocated)
 * @return 0 on success, negative error code on failure
 */
int halo_battery_register_callback(struct battery_callback *cb);

/**
 * @brief Unregister battery event callback
 * 
 * @param cb Pointer to callback structure to unregister
 * @return 0 on success, negative error code on failure
 */
int halo_battery_unregister_callback(struct battery_callback *cb);

/**
 * @brief Set battery update interval
 * 
 * Configures how often battery status is read from hardware.
 * 
 * @param interval_ms Update interval in milliseconds (0 to disable periodic updates)
 * @return 0 on success, negative error code on failure
 */
int halo_battery_set_update_interval(uint32_t interval_ms);

/**
 * @brief Force immediate battery status update
 * 
 * Triggers an immediate read from battery hardware.
 * 
 * @return 0 on success, negative error code on failure
 */
int halo_battery_update_now(void);

/**
 * @brief Suspend battery manager for low power mode
 * 
 * Prepares battery manager for system suspend.
 * 
 * @return 0 on success, negative error code on failure
 */
int halo_battery_suspend(void);

/**
 * @brief Resume battery manager after low power mode
 * 
 * Restores battery manager state after system resume.
 * 
 * @return 0 on success, negative error code on failure
 */
int halo_battery_resume(void);

#ifdef __cplusplus
}
#endif

#endif /* HALO_BATTERY_MANAGER_H */
