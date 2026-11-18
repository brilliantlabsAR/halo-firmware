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

#include <soc.h>

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

/*
 * Reset release. The camera driver's init leaves the reset asserted. A
 * sensor powered up from cold holds the shared I2C bus while in reset
 * and, if released too early, keeps holding it for seconds afterwards; a
 * release made after it has had CONFIG_HALO_CAMERA_RESET_HOLD_MS of power
 * frees the bus within milliseconds. A sensor that is not holding the bus
 * (the usual case) is released at once. Either way this runs between the
 * camera driver (CONFIG_VIDEO_INIT_PRIORITY) and the first other user of
 * the bus, the display regulator.
 */
#if DT_NODE_HAS_STATUS(DT_NODELABEL(gpio2), okay)

#define CAMERA_RESET_RELEASE_INIT_PRIORITY 60
#define CAMERA_BUS_IDLE_TIMEOUT_US         (50 * USEC_PER_MSEC)

/* The sensor's system clock (its CLKIN pin) comes from the SoC's camera
 * pixel clock output, which nothing switched on before: the sensor ran on
 * an internal fallback, and on some dies that fallback is slow enough
 * after a cold start that frames come out short and the controller never
 * completes one. Divider 0x14 of the camera pixel clock, enable bit set;
 * switched on before the reset release and left running while the sensor
 * is powered. The controller's pin control must have muxed the clock pad
 * by now (CONFIG_VIDEO_ALIF_CAM_INIT_PRIORITY, one priority earlier). */
#define CAMERA_XVCLK_DIVIDER 0x14U
#define CAMERA_XVCLK_ENABLE  BIT(0)
#define CAMERA_XVCLK_VALUE   ((CAMERA_XVCLK_DIVIDER << 16) | CAMERA_XVCLK_ENABLE)

#ifdef CONFIG_VIDEO_ALIF_CAM_INIT_PRIORITY
BUILD_ASSERT(CONFIG_VIDEO_ALIF_CAM_INIT_PRIORITY < CAMERA_RESET_RELEASE_INIT_PRIORITY,
	     "the camera controller must mux the clock pad before the reset release");
#endif

BUILD_ASSERT(CONFIG_VIDEO_INIT_PRIORITY < CAMERA_RESET_RELEASE_INIT_PRIORITY,
	     "the reset release must run after the camera driver's init");
#ifdef CONFIG_REGULATOR_TPS65132_COMMON_INIT_PRIORITY
BUILD_ASSERT(CAMERA_RESET_RELEASE_INIT_PRIORITY < CONFIG_REGULATOR_TPS65132_COMMON_INIT_PRIORITY,
	     "the reset release must run before the display regulator's init");
#endif

/* I2C1 SDA and SCL are P2_6 and P2_7 (board pin control); with the pad
 * read enable set, the GPIO port reads the line level whatever the mux. */
#define I2C1_LINES_MASK (BIT(6) | BIT(7))

static const struct device *const i2c1_lines_port = DEVICE_DT_GET(DT_NODELABEL(gpio2));

static bool i2c1_lines_idle(void)
{
	gpio_port_value_t lines;
	int ret = gpio_port_get_raw(i2c1_lines_port, &lines);

	if (ret != 0) {
		/* A port that cannot be read says nothing about the bus; do
		 * not turn a read error into a boot-long hold. */
		LOG_WRN("I2C bus lines unreadable (%d), assuming free", ret);
		return true;
	}
	return (lines & I2C1_LINES_MASK) == I2C1_LINES_MASK;
}

static int camera_reset_release(void)
{
	bool held;
	uint32_t start;
	uint32_t waited_us;

	if (!device_is_ready(cam_rst.port) || !device_is_ready(i2c1_lines_port)) {
		return 0;
	}
#ifdef HE_CAMERA_PIXCLK
	sys_write32(CAMERA_XVCLK_VALUE, HE_CAMERA_PIXCLK);
	k_busy_wait(200); /* a few hundred clock cycles before the release */
#endif
	held = !i2c1_lines_idle();
	if (held) {
		int64_t remaining = CONFIG_HALO_CAMERA_RESET_HOLD_MS - k_uptime_get();

		if (remaining > 0) {
			k_sleep(K_MSEC(remaining));
		}
	}
	gpio_pin_set_dt(&cam_rst, 0); /* release */
	if (!held) {
		return 0;
	}
	start = k_cycle_get_32();
	while (!i2c1_lines_idle()) {
		waited_us = k_cyc_to_us_floor32(k_cycle_get_32() - start);
		if (waited_us >= CAMERA_BUS_IDLE_TIMEOUT_US) {
			LOG_WRN("I2C bus still held %u ms after the camera reset release at %lld ms",
				waited_us / USEC_PER_MSEC, k_uptime_get());
			return 0;
		}
		k_busy_wait(100);
	}
	waited_us = k_cyc_to_us_floor32(k_cycle_get_32() - start);
	LOG_INF("I2C bus was held in reset; idle %u ms after the release at %lld ms",
		waited_us / USEC_PER_MSEC, k_uptime_get());
	return 0;
}

SYS_INIT(camera_reset_release, POST_KERNEL, CAMERA_RESET_RELEASE_INIT_PRIORITY);

#endif /* gpio2 */

#if DT_NODE_HAS_PROP(DT_NODELABEL(cam_1v8), enable_gpios)
static const struct gpio_dt_spec cam_en = GPIO_DT_SPEC_GET(DT_NODELABEL(cam_1v8), enable_gpios);

/* The enable pin's RC on the board is about 5 ms; ten times that. */
#define CAMERA_SLEEP_ENABLE_DRAIN_MS 50

void halo_camera_sleep_power_down(void)
{
	int ret;

	/* Reset first, so the sensor never sees its supplies fall with reset
	 * released. */
	(void)gpio_pin_configure_dt(&cam_rst, GPIO_OUTPUT_ACTIVE);
	/* The regulator API cannot switch an always-on supply off; the core
	 * is about to power down and take the pad with it anyway. */
	ret = gpio_pin_configure_dt(&cam_en, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("Camera rail disable failed: %d", ret);
		return;
	}
	k_sleep(K_MSEC(CAMERA_SLEEP_ENABLE_DRAIN_MS));
	LOG_DBG("Camera rails off for sleep");
}
#else
void halo_camera_sleep_power_down(void)
{
}
#endif

#else

void halo_camera_sleep_power_down(void)
{
}

#endif
