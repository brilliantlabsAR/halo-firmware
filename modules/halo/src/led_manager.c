/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>

#include <halo/led_manager.h>
#include <halo/pm_manager.h>

LOG_MODULE_REGISTER(led_manager, CONFIG_HALO_LOG_LEVEL);

/* LED manager context */
static struct {
	bool initialized;

	/* LED device */
	const struct device *led_dev;

	/* PM callback structure */
	struct halo_pm_callback pm_cb;

	/* Current active state and priority */
	halo_led_state_t current_state;
	halo_led_priority_t current_priority;

	/* State tracking per priority */
	halo_led_state_t priority_states[HALO_LED_PRIORITY_LOW + 1];

	/* Saved state during sleep */
	halo_led_state_t saved_state;
	halo_led_priority_t saved_priority;
	bool state_saved;
} led_mgr_ctx = {
	.initialized = false,
	.current_state = HALO_LED_STATE_OFF,
	.current_priority = HALO_LED_PRIORITY_LOW,
	.saved_state = HALO_LED_STATE_OFF,
	.saved_priority = HALO_LED_PRIORITY_LOW,
	.state_saved = false,
};

/* Blink periods for different states */
#define BLINK_SLOW_ON_MS  500
#define BLINK_SLOW_OFF_MS 500
#define BLINK_FAST_ON_MS  250
#define BLINK_FAST_OFF_MS 250

/* Forward declarations */
static void update_led(void);

#if CONFIG_HALO_PM_MANAGER
/**
 * @brief PM callback handler for LED power management
 */
static int led_pm_callback_handler(halo_pm_event_t event, halo_pm_sleep_mode_t mode,
				   void *user_data)
{
	ARG_UNUSED(mode);
	ARG_UNUSED(user_data);

	switch (event) {
	case HALO_PM_EVENT_SUSPEND:
		/* Save current state and turn off LED */
		led_mgr_ctx.saved_state = led_mgr_ctx.current_state;
		led_mgr_ctx.saved_priority = led_mgr_ctx.current_priority;
		led_mgr_ctx.state_saved = true;

		/* Turn off LED completely */
		led_off(led_mgr_ctx.led_dev, 0);

		LOG_DBG("LED suspended - saved state: %d prio: %d", led_mgr_ctx.saved_state,
			led_mgr_ctx.saved_priority);
		return 0; /* Always suspend LED (needs resume) */

	case HALO_PM_EVENT_RESUME:
		/* Note: PM manager will only call this if we returned 0 from suspend */
		/* Restore previous state */
		led_mgr_ctx.current_state = led_mgr_ctx.saved_state;
		led_mgr_ctx.current_priority = led_mgr_ctx.saved_priority;
		update_led();
		led_mgr_ctx.state_saved = false;

		LOG_DBG("LED resumed - restored state: %d prio: %d", led_mgr_ctx.current_state,
			led_mgr_ctx.current_priority);
		return 0;

	default:
		break;
	}

	return 0;
}
#endif

/**
 * @brief Update LED based on current state
 */
static void update_led(void)
{
	switch (led_mgr_ctx.current_state) {
	case HALO_LED_STATE_OFF:
		led_off(led_mgr_ctx.led_dev, 0);
		break;

	case HALO_LED_STATE_ON:
		led_on(led_mgr_ctx.led_dev, 0);
		break;

	case HALO_LED_STATE_BLINK_SLOW:
		led_blink(led_mgr_ctx.led_dev, 0, BLINK_SLOW_ON_MS, BLINK_SLOW_OFF_MS);
		break;

	case HALO_LED_STATE_BLINK_FAST:
		led_blink(led_mgr_ctx.led_dev, 0, BLINK_FAST_ON_MS, BLINK_FAST_OFF_MS);
		break;

	case HALO_LED_STATE_CHARGING:
		/* Breathing effect - blink(0,0) triggers breathing mode in driver */
		led_blink(led_mgr_ctx.led_dev, 0, 0, 0);
		break;

	case HALO_LED_STATE_PAIRING:
		/* Pairing pattern - 1Hz blink */
		led_blink(led_mgr_ctx.led_dev, 0, BLINK_SLOW_ON_MS, BLINK_SLOW_OFF_MS);
		break;
	}
}

/**
 * @brief Find the highest priority active state
 */
static void find_highest_priority_state(void)
{
	halo_led_priority_t highest_prio = HALO_LED_PRIORITY_LOW;
	halo_led_state_t highest_state = HALO_LED_STATE_OFF;

	for (int prio = HALO_LED_PRIORITY_HIGH; prio <= HALO_LED_PRIORITY_LOW; prio++) {
		if (led_mgr_ctx.priority_states[prio] != HALO_LED_STATE_OFF) {
			highest_prio = prio;
			highest_state = led_mgr_ctx.priority_states[prio];
			break;
		}
	}

	led_mgr_ctx.current_state = highest_state;
	led_mgr_ctx.current_priority = highest_prio;
	update_led();
}

int halo_led_init(void)
{
	/* Get LED device */
	led_mgr_ctx.led_dev = DEVICE_DT_GET_ONE(pwm_leds);
	if (!device_is_ready(led_mgr_ctx.led_dev)) {
		LOG_ERR("LED device not ready");
		return -ENODEV;
	}

	/* Initialize state tracking */
	for (int i = 0; i <= HALO_LED_PRIORITY_LOW; i++) {
		led_mgr_ctx.priority_states[i] = HALO_LED_STATE_OFF;
	}

	/* Set initial state */
	led_mgr_ctx.current_state = HALO_LED_STATE_OFF;
	led_mgr_ctx.current_priority = HALO_LED_PRIORITY_LOW;

#ifdef CONFIG_HALO_PM_MANAGER
	/* Register PM callback */
	int ret = halo_pm_register_callback(&led_mgr_ctx.pm_cb, led_pm_callback_handler, NULL,
					    "led_manager", 20); /* Medium priority for LED */
	if (ret != 0) {
		LOG_ERR("Failed to register LED PM callback: %d", ret);
		return ret;
	}
#endif

	LOG_DBG("LED manager initialized");
	return 0;
}

int halo_led_set_state(halo_led_state_t state, halo_led_priority_t priority)
{
	if (priority > HALO_LED_PRIORITY_LOW || priority < HALO_LED_PRIORITY_HIGH) {
		return -EINVAL;
	}

	/* Set state for this priority */
	led_mgr_ctx.priority_states[priority] = state;

	/* Update current state if this priority is higher or equal */
	if (priority <= led_mgr_ctx.current_priority) {
		find_highest_priority_state();
	}

	return 0;
}

int halo_led_clear_state(halo_led_priority_t priority)
{
	if (priority > HALO_LED_PRIORITY_LOW || priority < HALO_LED_PRIORITY_HIGH) {
		return -EINVAL;
	}

	/* Clear state for this priority */
	led_mgr_ctx.priority_states[priority] = HALO_LED_STATE_OFF;

	/* Find new highest priority state */
	find_highest_priority_state();

	return 0;
}

int halo_led_get_current_state(halo_led_state_t *state, halo_led_priority_t *priority)
{
	if (!state || !priority) {
		return -EINVAL;
	}

	*state = led_mgr_ctx.current_state;
	*priority = led_mgr_ctx.current_priority;

	return 0;
}