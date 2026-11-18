/*
 * Camera sensor reset ordering at boot.
 *
 * The sensor's supplies are switched on by the fixed-regulator init. Before
 * this step the reset pad was left as an input from pin control until the
 * camera driver's init, so the reset line simply followed the supply up
 * through the module's pull-up: reset released at the same instant as power,
 * with no hold time. After a wake from the powered-off core state the sensor
 * then came up holding the shared I2C bus for anything up to seconds, and
 * the display, which shares that bus, failed its first power-up.
 *
 * Asserting the reset here, one priority ahead of the regulators, keeps it
 * low through the whole supply ramp. The camera driver's init releases it
 * with its usual pulse a few milliseconds later.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include <halo/camera_boot.h>

LOG_MODULE_REGISTER(camera_boot, CONFIG_HALO_LOG_LEVEL);

#if DT_NODE_HAS_PROP(DT_NODELABEL(pag7982), reset_gpios)

/* One ahead of the fixed regulators (CONFIG_REGULATOR_FIXED_INIT_PRIORITY,
 * 32); SYS_INIT needs a plain number here. */
#define CAMERA_RESET_HOLD_INIT_PRIORITY 31

BUILD_ASSERT(CAMERA_RESET_HOLD_INIT_PRIORITY < CONFIG_REGULATOR_FIXED_INIT_PRIORITY,
	     "camera reset hold must run ahead of the fixed regulators");

static const struct gpio_dt_spec cam_rst = GPIO_DT_SPEC_GET(DT_NODELABEL(pag7982), reset_gpios);

static int camera_reset_hold_init(void)
{
	int ret;

	if (!device_is_ready(cam_rst.port)) {
		LOG_ERR("Camera reset GPIO not ready");
		return 0;
	}
	ret = gpio_pin_configure_dt(&cam_rst, GPIO_OUTPUT_ACTIVE); /* assert */
	if (ret < 0) {
		LOG_ERR("Camera reset assert failed: %d", ret);
	}
	return 0;
}

SYS_INIT(camera_reset_hold_init, POST_KERNEL, CAMERA_RESET_HOLD_INIT_PRIORITY);

#endif
