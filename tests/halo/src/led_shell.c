#include <string.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#ifdef CONFIG_LED_PWM
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <errno.h>
#include <zephyr/drivers/led.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

#define LED_PWM_NODE_ID	 DT_COMPAT_GET_ANY_STATUS_OKAY(pwm_leds)

static const struct device *led_pwm = NULL;


static int cmd_get_device(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!led_pwm) {
		shell_error(sh, "No led device found");
		return -ENODEV;
	}

	shell_print(sh, "led Device: %s", led_pwm->name);

	return err;
}

static int cmd_led_init(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	led_pwm = DEVICE_DT_GET(LED_PWM_NODE_ID);

	if (!device_is_ready(led_pwm)) {
		shell_error(sh, "led_pwm device %s not ready", led_pwm->name);
		return -1;
	}

	return err;
}

static int cmd_led_brightness(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	uint8_t brightness = 0;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!led_pwm) {
		shell_error(sh, "No led device found");
		return -ENODEV;
	}

	brightness = strtol(argv[1], NULL, 10);
	shell_print(sh, "Brightness: %d", brightness);
	led_set_brightness(led_pwm, 0, brightness);

	return err;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_led,
	SHELL_CMD_ARG(get_device, NULL, "get the led device", cmd_get_device, 1, 0),
	SHELL_CMD_ARG(init, NULL, "initialize the led device", cmd_led_init, 1, 0),
	SHELL_CMD_ARG(brightness, NULL, "set the brightness of the led device",
		      cmd_led_brightness, 2, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(led, &sub_led, "led device commands", NULL);

#endif