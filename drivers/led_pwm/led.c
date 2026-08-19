/*
 * Copyright (c) 2020 Seagate Technology LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT pwm_leds

/**
 * @file
 * @brief PWM driven LEDs
 */

#include <zephyr/drivers/led.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/device.h>
#include <zephyr/pm/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/math_extras.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(led_pwm, CONFIG_LOG_DEFAULT_LEVEL);

#define USE_TIMER

struct led_pwm_config {
	int num_leds;
	const struct pwm_dt_spec *led;
};

#ifdef USE_TIMER
static int led_pwm_set_brightness(const struct device *dev, uint32_t led, uint8_t value);
static uint8_t blink_value = 0;
static uint32_t delay_on = 0; 
static uint32_t delay_off = 0; 

/* Breathing state */
static bool breathing_mode = false;
static uint8_t breathing_brightness = 5;
static bool breathing_up = true;

#define BREATHING_STEP_MS      20    /* Time between brightness steps */
#define BREATHING_MIN          5     /* Minimum brightness (visible threshold) */
#define BREATHING_MAX          90    /* Maximum brightness */

static void blink_timeout(struct k_timer *timer) {
    const struct device *dev = k_timer_user_data_get(timer);
    
    if (breathing_mode) {
        /* Simple breathing: just go up and down */
        if (breathing_up) {
            breathing_brightness++;
            if (breathing_brightness >= BREATHING_MAX) {
                breathing_up = false;
            }
        } else {
            breathing_brightness--;
            if (breathing_brightness <= BREATHING_MIN) {
                breathing_up = true;
            }
        }
        
        led_pwm_set_brightness(dev, 0, breathing_brightness);
		/* Periodic timer handles rescheduling for breathing */
    } else {
		/* Guard against stale timer expiry after leaving breathing mode.
		 * If no valid blink delays are configured, stop here. */
		if (delay_on == 0 && delay_off == 0) {
			k_timer_stop(timer);
			return;
		}

        /* Normal blink mode */
        if(blink_value) {
            blink_value = 0;
            led_pwm_set_brightness(dev, 0, 100); 
            k_timer_start(timer, K_MSEC(delay_on), K_NO_WAIT); 
        } else {
            blink_value = 1;
            led_pwm_set_brightness(dev, 0, 0); 
            k_timer_start(timer, K_MSEC(delay_off), K_NO_WAIT); 
        }
    }
}

static K_TIMER_DEFINE(blink_timer, blink_timeout, NULL);

static int led_pwm_blink(const struct device *dev, uint32_t led,
             uint32_t delay_on_val, uint32_t delay_off_val)
{
    k_timer_user_data_set(&blink_timer, (void *)dev);

    /* Magic value: delay_on=0, delay_off=0 => breathing mode */
    if (delay_on_val == 0 && delay_off_val == 0) {
        if (breathing_mode) {
            /* Already breathing, don't restart */
            return 0;
        }
		delay_on = 0;
		delay_off = 0;
		blink_value = 0;
        breathing_mode = true;
        breathing_brightness = BREATHING_MIN;
        breathing_up = true;
        led_pwm_set_brightness(dev, 0, BREATHING_MIN);
		/* Start periodic timer for breathing cadence */
		k_timer_start(&blink_timer, K_MSEC(BREATHING_STEP_MS), K_MSEC(BREATHING_STEP_MS));
        return 0;
    }

    /* Normal blink mode */
    breathing_mode = false;
    delay_on = delay_on_val;
    delay_off = delay_off_val;
    
    blink_value = 1;
    led_pwm_set_brightness(dev, 0, 0); 
    k_timer_start(&blink_timer, K_MSEC(delay_on), K_NO_WAIT);
    
    return 0; 
}

#elif
static int led_pwm_blink(const struct device *dev, uint32_t led,
			 uint32_t delay_on, uint32_t delay_off)
{
	const struct led_pwm_config *config = dev->config;
	const struct pwm_dt_spec *dt_led;
	uint32_t period_usec, pulse_usec;

	if (led >= config->num_leds) {
		return -EINVAL;
	}

	/*
	 * Convert delays (in ms) into PWM period and pulse (in us) and check
	 * for overflows.
	 */
	if (u32_add_overflow(delay_on, delay_off, &period_usec) ||
	    u32_mul_overflow(period_usec, 1000, &period_usec) ||
	    u32_mul_overflow(delay_on, 1000, &pulse_usec)) {
		return -EINVAL;
	}

	dt_led = &config->led[led];
	
	printf("%d ms\n", PWM_USEC(period_usec)/1000);
	return pwm_set_dt(dt_led, PWM_USEC(period_usec), PWM_USEC(period_usec)-PWM_USEC(pulse_usec));
}
#endif

static int led_pwm_set_brightness(const struct device *dev,
				  uint32_t led, uint8_t value)
{
	const struct led_pwm_config *config = dev->config;
	const struct pwm_dt_spec *dt_led;

	if (led >= config->num_leds || value > 100) {
		return -EINVAL;
	}

	/* Map to duty cycle (active-low LED). */
	value = 100 - value;

	dt_led = &config->led[led];

	return pwm_set_pulse_dt(&config->led[led],
			(uint32_t) ((uint64_t) dt_led->period * value / 100));
}

static int led_pwm_on(const struct device *dev, uint32_t led)
{
#ifdef USE_TIMER
	k_timer_stop(&blink_timer);
	breathing_mode = false;
	delay_on = 0;
	delay_off = 0;
	blink_value = 0;
#endif
	return led_pwm_set_brightness(dev, led, 100);
}

static int led_pwm_off(const struct device *dev, uint32_t led)
{
#ifdef USE_TIMER
	k_timer_stop(&blink_timer);
	breathing_mode = false;
	delay_on = 0;
	delay_off = 0;
	blink_value = 0;
#endif
	return led_pwm_set_brightness(dev, led, 0);
}

static int led_pwm_init(const struct device *dev)
{
	const struct led_pwm_config *config = dev->config;
	int i;

	if (!config->num_leds) {
		LOG_ERR("%s: no LEDs found (DT child nodes missing)",
			dev->name);
		return -ENODEV;
	}

	for (i = 0; i < config->num_leds; i++) {
		const struct pwm_dt_spec *led = &config->led[i];

		if (!device_is_ready(led->dev)) {
			LOG_ERR("%s: pwm device not ready", led->dev->name);
			return -ENODEV;
		}
	}

	LOG_INF("LED PWM Driver Init.");

	return 0;
}

#ifdef CONFIG_PM_DEVICE
static int led_pwm_pm_action(const struct device *dev,
			     enum pm_device_action action)
{
	const struct led_pwm_config *config = dev->config;

	/* switch all underlying PWM devices to the new state */
	for (size_t i = 0; i < config->num_leds; i++) {
		int err;
		const struct pwm_dt_spec *led = &config->led[i];

		LOG_DBG("PWM %p running pm action %" PRIu32, led->dev, action);

		err = pm_device_action_run(led->dev, action);
		if (err && (err != -EALREADY)) {
			LOG_DBG("Cannot switch PWM %p power state (err = %d)", led->dev, err);
		}
	}

	return 0;
}
#endif /* CONFIG_PM_DEVICE */

static const struct led_driver_api led_pwm_api = {
	.on		= led_pwm_on,
	.off		= led_pwm_off,
	.blink		= led_pwm_blink,
	.set_brightness	= led_pwm_set_brightness,
};

#define LED_PWM_DEVICE(id)					\
								\
static const struct pwm_dt_spec led_pwm_##id[] = {		\
	DT_INST_FOREACH_CHILD_SEP(id, PWM_DT_SPEC_GET, (,))	\
};								\
								\
static const struct led_pwm_config led_pwm_config_##id = {	\
	.num_leds	= ARRAY_SIZE(led_pwm_##id),		\
	.led		= led_pwm_##id,				\
};								\
								\
PM_DEVICE_DT_INST_DEFINE(id, led_pwm_pm_action);		\
								\
DEVICE_DT_INST_DEFINE(id, &led_pwm_init,			\
		      PM_DEVICE_DT_INST_GET(id), NULL,		\
		      &led_pwm_config_##id, POST_KERNEL,	\
		      CONFIG_LED_INIT_PRIORITY, &led_pwm_api);

DT_INST_FOREACH_STATUS_OKAY(LED_PWM_DEVICE)
