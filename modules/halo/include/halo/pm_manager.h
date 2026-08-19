/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_PM_MANAGER_H
#define HALO_PM_MANAGER_H

#include <zephyr/kernel.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Power management sleep modes
 */
typedef enum {
	HALO_PM_SLEEP_STANDBY, /**< Standby sleep - BLE on, quick wake, partial power down */
	HALO_PM_SLEEP_LIGHT,   /**< Light sleep - BLE on, quick wake */
	HALO_PM_SLEEP_DEEP,    /**< Deep sleep - BLE off, Only BTN pressed wakes the device */
	HALO_PM_SLEEP_NONE,     /**< Not sleeping */
} halo_pm_sleep_mode_t;

/**
 * @brief Wakeup sources
 */
typedef enum {
	HALO_PM_WAKEUP_UNKNOWN = 0, /**< Unknown wakeup source */
	HALO_PM_WAKEUP_TIMEOUT,     /**< Wakeup by timeout */
	HALO_PM_WAKEUP_BUTTON,      /**< Wakeup by button press */
	HALO_PM_WAKEUP_BLE,         /**< Wakeup by BLE data */
	HALO_PM_WAKEUP_IMU,         /**< Wakeup by IMU interrupt */
	HALO_PM_WAKEUP_MICROPHONE,  /**< Wakeup by microphone interrupt */
	HALO_PM_WAKEUP_EXTERNAL,    /**< Wakeup by external interrupt */
	HALO_PM_WAKEUP_WATCHDOG,    /**< Wakeup by watchdog timer */
} halo_pm_wakeup_source_t;

/**
 * @brief Power management callback events
 */
typedef enum {
	HALO_PM_EVENT_SUSPEND, /**< Before entering sleep */
	HALO_PM_EVENT_RESUME,  /**< After waking from sleep */
} halo_pm_event_t;

/**
 * @brief Power management callback function
 *
 * @param event PM event type (suspend or resume)
 * @param mode Sleep mode (light or deep)
 * @param user_data User data pointer passed during registration
 * @return For SUSPEND event:
 *         - 0 = Successfully suspended (will be resumed on wake)
 *         - 1 = Skipped (device already suspended, will NOT be resumed)
 *         - <0 = Error
 *         For RESUME event:
 *         - 0 = Successfully resumed
 *         - <0 = Error (logged but doesn't stop other callbacks)
 */
typedef int (*halo_pm_callback_t)(halo_pm_event_t event, halo_pm_sleep_mode_t mode,
				  void *user_data);

/**
 * @brief Power management callback node
 */
/* PM callback structure */
struct halo_pm_callback {
	sys_dnode_t node; /* Changed from sys_snode_t to support reverse iteration */
	halo_pm_callback_t callback;
	void *user_data;
	const char *name;
	int priority;
	bool suspended; /* Internal: tracks if this callback suspended on last sleep */
};

/**
 * @brief Initialize the power management system
 *
 * @return 0 on success, negative error code on failure
 */
int halo_pm_init(void);

/**
 * @brief Register a power management callback
 *
 * Callbacks are called in priority order on suspend (highest first)
 * and in reverse priority order on resume (lowest first).
 *
 * @param cb Callback structure (must remain valid)
 * @param callback Callback function
 * @param user_data User data pointer
 * @param name Callback name for debugging
 * @param priority Priority (0-100, higher = earlier on suspend)
 * @return 0 on success, negative error code on failure
 */
int halo_pm_register_callback(struct halo_pm_callback *cb, halo_pm_callback_t callback,
			      void *user_data, const char *name, int priority);

/**
 * @brief Unregister a power management callback
 *
 * @param cb Callback structure
 * @return 0 on success, negative error code on failure
 */
int halo_pm_unregister_callback(struct halo_pm_callback *cb);

/**
 * @brief Enter light sleep mode
 *
 * Suspends peripherals and enters low power state.
 * Quick wake time, suitable for short idle periods.
 *
 * @param timeout_ms Maximum sleep duration in milliseconds (0 = indefinite)
 * @return 0 on success, negative error code on failure
 */
int halo_pm_sleep_light(uint32_t timeout_ms);

/**
 * @brief Enter standby sleep mode
 *
 * Suspends peripherals and enters low power state.
 * Quick wake time, suitable for short idle periods.
 *
 * @param timeout_ms Maximum sleep duration in milliseconds (0 = indefinite)
 * @return 0 on success, negative error code on failure
 */
int halo_pm_sleep_standby(uint32_t timeout_ms);

/**
 * @brief Schedule standby sleep mode asynchronously
 *
 * Submits a work item to enter standby sleep mode. This is useful when
 * calling from interrupt context or when the caller cannot block.
 *
 * @param timeout_ms Maximum sleep duration in milliseconds (0 = indefinite)
 * @return 0 on success, negative error code on failure
 */
int halo_pm_sleep_standby_async(uint32_t timeout_ms);

/**
 * @brief Enter deep sleep mode
 *
 * Suspends all peripherals and enters deepest low power state.
 * Longer wake time, suitable for extended idle periods.
 *
 * @param timeout_ms Maximum sleep duration in milliseconds (0 = indefinite)
 * @return 0 on success, negative error code on failure
 */
int halo_pm_sleep_deep(uint32_t timeout_ms);

/**
 * @brief Get current power management statistics
 *
 * @param light_count Pointer to store light sleep count
 * @param standby_count Pointer to store standby sleep count
 * @param deep_count Pointer to store deep sleep count
 * @param last_mode Pointer to store last sleep mode used
 * @return 0 on success, negative error code on failure
 */
int halo_pm_get_stats(uint32_t *light_count, uint32_t *standby_count, uint32_t *deep_count,
		      halo_pm_sleep_mode_t *last_mode);

/**
 * @brief Check if system is in light-sleep mode
 *
 * @return true if sleeping, false otherwise
 */
bool halo_pm_is_sleeping(void);

/**
 * @brief Wakeup from light-sleep mode
 *
 * Can be called from button interrupt, BLE event, or any wakeup source.
 * If not in light-sleep mode, this function does nothing.
 *
 * @param source Wakeup source indicator
 * @return 0 on success, negative error code on failure
 */
int halo_pm_wakeup(halo_pm_wakeup_source_t source);

/**
 * @brief Get the last wakeup source
 *
 * @return Last wakeup source
 */
halo_pm_wakeup_source_t halo_pm_get_wakeup_source(void);

/**
 * @brief Get wakeup source name string
 *
 * @param source Wakeup source
 * @return Name string
 */
const char *halo_pm_wakeup_source_name(halo_pm_wakeup_source_t source);

/**
 * @brief Prevent the system from entering sleep
 *
 * Increments the sleep lock counter. Each call to this function
 * must be matched with a call to halo_pm_allow_sleep().
 *
 * @return 0 on success, negative error code on failure
 */
int halo_pm_prevent_sleep(void);

/**
 * @brief Allow the system to enter sleep
 *
 * Decrements the sleep lock counter. When the counter reaches zero,
 * the system is allowed to enter sleep modes.
 *
 * @return 0 on success, negative error code on failure
 */
int halo_pm_allow_sleep(void);


/**
 * @brief Get the last sleep mode used
 *
 * @return Last sleep mode
 */
halo_pm_sleep_mode_t halo_pm_get_sleep_mode(void);

/**
 * @brief Helper macro to define a PM callback statically
 *
 * @param _name Variable name
 * @param _cb_func Callback function
 * @param _user_data User data pointer
 * @param _cb_name Callback name string
 * @param _prio Priority (0-100)
 */
#define HALO_PM_CALLBACK_DEFINE(_name, _cb_func, _user_data, _cb_name, _prio)                      \
	static struct halo_pm_callback _name = {                                                   \
		.callback = _cb_func,                                                              \
		.user_data = _user_data,                                                           \
		.name = _cb_name,                                                                  \
		.priority = _prio,                                                                 \
	}

#ifdef __cplusplus
}
#endif

#endif /* HALO_PM_MANAGER_H */
