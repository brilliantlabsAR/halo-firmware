/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_LED_MANAGER_H
#define HALO_LED_MANAGER_H

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LED states
 */
typedef enum {
	HALO_LED_STATE_OFF,           /**< LED off */
	HALO_LED_STATE_ON,            /**< LED on */
	HALO_LED_STATE_BLINK_SLOW,    /**< Slow blink (1Hz) */
	HALO_LED_STATE_BLINK_FAST,    /**< Fast blink (2Hz) */
	HALO_LED_STATE_CHARGING,      /**< Charging indicator */
	HALO_LED_STATE_PAIRING,       /**< Pairing mode indicator */
} halo_led_state_t;

/**
 * @brief LED priority levels (lower number = higher priority)
 */
typedef enum {
	HALO_LED_PRIORITY_LOW = 10,     /**< Low priority (e.g., status) */
	HALO_LED_PRIORITY_MEDIUM = 5,   /**< Medium priority (e.g., charging) */
	HALO_LED_PRIORITY_HIGH = 1,     /**< High priority (e.g., pairing, error) */
} halo_led_priority_t;

/**
 * @brief Initialize the LED manager
 * 
 * @return 0 on success, negative error code on failure
 */
int halo_led_init(void);/**
 * @brief Set LED state with priority
 *
 * Only sets the state if the priority is higher than current active priority.
 * Higher priority (lower number) overrides lower priority states.
 *
 * @param state LED state to set
 * @param priority Priority level
 * @return 0 on success, negative error code on failure
 */
int halo_led_set_state(halo_led_state_t state, halo_led_priority_t priority);

/**
 * @brief Clear LED state for a specific priority
 *
 * Removes the state associated with the given priority and falls back
 * to the next highest priority state.
 *
 * @param priority Priority level to clear
 * @return 0 on success, negative error code on failure
 */
int halo_led_clear_state(halo_led_priority_t priority);

/**
 * @brief Get current LED state and priority
 *
 * @param state Pointer to store current state
 * @param priority Pointer to store current priority
 * @return 0 on success, negative error code on failure
 */
int halo_led_get_current_state(halo_led_state_t *state, halo_led_priority_t *priority);

#ifdef __cplusplus
}
#endif

#endif /* HALO_LED_MANAGER_H */