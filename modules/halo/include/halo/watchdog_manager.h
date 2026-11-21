/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_WATCHDOG_MANAGER_H
#define HALO_WATCHDOG_MANAGER_H

#include <zephyr/kernel.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Watchdog callback function type
 *
 * Called before watchdog reset if enabled.
 * Should be quick and avoid blocking operations.
 *
 * @param user_data User data pointer passed during initialization
 */
typedef void (*halo_watchdog_callback_t)(void *user_data);

/**
 * @brief Initialize the watchdog manager
 *
 * Sets up the watchdog with configured timeout and callback.
 * Must be called before using other watchdog functions.
 *
 * @return 0 on success, negative error code on failure
 */
int halo_watchdog_init(void);

/**
 * @brief Feed the watchdog
 *
 * Resets the watchdog timer to prevent system reset.
 * Should be called periodically to indicate system is healthy.
 *
 * @return 0 on success, negative error code on failure
 */
int halo_watchdog_feed(void);

/**
 * @brief Set watchdog timeout
 *
 * Changes the watchdog timeout period.
 * Only effective if supported by hardware.
 *
 * @param timeout_ms New timeout in milliseconds
 * @return 0 on success, negative error code on failure
 */
int halo_watchdog_set_timeout(uint32_t timeout_ms);

/**
 * @brief Get current watchdog timeout
 *
 * @param timeout_ms Pointer to store current timeout in milliseconds
 * @return 0 on success, negative error code on failure
 */
int halo_watchdog_get_timeout(uint32_t *timeout_ms);

/**
 * @brief Register a callback to be called before reset
 *
 * The callback is called when watchdog is about to reset the system.
 * Only one callback can be registered.
 *
 * @param callback Callback function (NULL to disable)
 * @param user_data User data pointer passed to callback
 * @return 0 on success, negative error code on failure
 */
int halo_watchdog_register_callback(halo_watchdog_callback_t callback, void *user_data);

/**
 * @brief Suspend the watchdog
 *
 * Temporarily suspends the watchdog timer. Useful during sleep modes.
 * Note: Hardware support required for actual suspending.
 *
 * @return 0 on success, negative error code on failure
 */
int halo_watchdog_suspend(void);

/**
 * @brief Resume the watchdog
 *
 * Resumes the watchdog timer after suspending.
 *
 * @return 0 on success, negative error code on failure
 */
int halo_watchdog_resume(void);


/**
 * @brief Stop the automatic feeding once a deadline passes
 *
 * Until @p deadline_ms (k_uptime_get_32() time base) the feed thread keeps
 * feeding as usual; after it the thread stops, logs @p reason once and lets
 * the hardware watchdog reset the SoC. With the pre-reset callback enabled
 * the watchdog interrupts at its first timeout and resets at the second, so
 * the reset lands about two timeout periods after feeding stops. The reset
 * cannot be blocked by any thread, mutex, workqueue or interrupt lock, which
 * makes this the failsafe for a sequence that must finish or the device is
 * lost, such as the deep-sleep suspend chain.
 *
 * @param deadline_ms Uptime after which feeding stops
 * @param reason Logged when feeding stops (must stay valid until then)
 * @return 0 on success, negative errno on failure
 */
int halo_watchdog_feed_until(uint32_t deadline_ms, const char *reason);

/**
 * @brief Cancel a feed deadline set by halo_watchdog_feed_until()
 */
void halo_watchdog_feed_unbounded(void);

/**
 * @brief Check if the watchdog has fired
 *
 * @return true if the watchdog has fired since last check, false otherwise
 */
bool halo_watchdog_has_fired(void);

#ifdef __cplusplus
}
#endif

#endif /* HALO_WATCHDOG_MANAGER_H */