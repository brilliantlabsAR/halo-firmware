/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/pm/pm.h>

#include "halo/watchdog_manager.h"
#include "halo/pm_manager.h"

LOG_MODULE_REGISTER(halo_watchdog, CONFIG_HALO_LOG_LEVEL);

#define HALO_WATCHDOG_FIRED_MAGIC 0x57444649 /* 'WDFI' */

/* Watchdog manager context */
static struct {
	bool initialized;
	const struct device *wdt_dev;
	int wdt_channel_id;
	bool handled_event;
	halo_watchdog_callback_t callback;
	void *callback_user_data;
	uint32_t timeout_ms;
#ifdef CONFIG_HALO_WATCHDOG_AUTO_FEED
	struct k_thread feed_thread;
	K_THREAD_STACK_MEMBER(feed_stack, CONFIG_HALO_WATCHDOG_AUTO_FEED_STACK_SIZE);
#endif
#ifdef CONFIG_HALO_PM_MANAGER
	struct halo_pm_callback pm_cb;
#endif
} wdt_ctx;

static int32_t watchdog_fired __attribute__((noinit));

#if CONFIG_HALO_WATCHDOG_ALLOW_CALLBACK
static void wdt_callback(const struct device *wdt_dev, int channel_id)
{
	if (wdt_ctx.handled_event) {
		return;
	}
	watchdog_fired = HALO_WATCHDOG_FIRED_MAGIC;
	if (wdt_ctx.callback) {
		wdt_ctx.callback(wdt_ctx.callback_user_data);
	}

	// wdt_feed(wdt_dev, channel_id);
	// wdt_disable(wdt_dev);  // Disable to prevent reset

	// printf("Watchdog timeout occurred! System will reboot now.\n");
	//  wdt_ctx.handled_event = true;
	//  sys_reboot(SYS_REBOOT_COLD);  // Force reboot after handling
}
#endif /* CONFIG_HALO_WATCHDOG_ALLOW_CALLBACK */

#ifdef CONFIG_HALO_WATCHDOG_AUTO_FEED
static void watchdog_feed_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		halo_watchdog_feed();
		k_msleep(CONFIG_HALO_WATCHDOG_FEED_INTERVAL_MS);
	}
}
#endif /* CONFIG_HALO_WATCHDOG_AUTO_FEED */

#ifdef CONFIG_HALO_PM_MANAGER
static int watchdog_pm_callback(halo_pm_event_t event, halo_pm_sleep_mode_t mode, void *user_data)
{
	ARG_UNUSED(user_data);

	switch (event) {
	case HALO_PM_EVENT_SUSPEND:
		if (mode == HALO_PM_SLEEP_DEEP || mode == HALO_PM_SLEEP_LIGHT) {
			halo_watchdog_suspend();
			return 0; /* Suspended - needs resume */
		}
		return 1; /* Skip for standby mode */
	case HALO_PM_EVENT_RESUME:
		/* Note: PM manager will only call this if we returned 0 from suspend */
		halo_watchdog_resume();
		return 0;
	default:
		break;
	}

	return 1; /* Skip by default */
}
#endif /* CONFIG_HALO_PM_MANAGER */

int halo_watchdog_init(void)
{
	int err;
	struct wdt_timeout_cfg wdt_config;

	if (wdt_ctx.initialized) {
		return 0;
	}

	wdt_ctx.wdt_dev = DEVICE_DT_GET(DT_ALIAS(watchdog0));
	if (!device_is_ready(wdt_ctx.wdt_dev)) {
		LOG_ERR("Watchdog device not ready");
		return -ENODEV;
	}

	wdt_ctx.timeout_ms = CONFIG_HALO_WATCHDOG_TIMEOUT_MS;

	/* Configure watchdog timeout */
	wdt_config.flags = WDT_FLAG_RESET_SOC;
	wdt_config.window.min = 0U;
	wdt_config.window.max = wdt_ctx.timeout_ms;

#if CONFIG_HALO_WATCHDOG_ALLOW_CALLBACK
	wdt_config.callback = wdt_callback;
#else
	wdt_config.callback = NULL;
#endif

	wdt_ctx.wdt_channel_id = wdt_install_timeout(wdt_ctx.wdt_dev, &wdt_config);
	if (wdt_ctx.wdt_channel_id < 0) {
		LOG_ERR("Failed to install watchdog timeout: %d", wdt_ctx.wdt_channel_id);
		return wdt_ctx.wdt_channel_id;
	}

	/* Setup watchdog */
	err = wdt_setup(wdt_ctx.wdt_dev, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (err < 0) {
		LOG_ERR("Failed to setup watchdog: %d", err);
		return err;
	}

	wdt_ctx.initialized = true;

#ifdef CONFIG_HALO_WATCHDOG_AUTO_FEED
	/* Start automatic feeding thread */
	k_thread_create(&wdt_ctx.feed_thread, wdt_ctx.feed_stack,
			K_THREAD_STACK_SIZEOF(wdt_ctx.feed_stack), watchdog_feed_thread, NULL, NULL,
			NULL, K_PRIO_COOP(CONFIG_HALO_WATCHDOG_AUTO_FEED_PRIORITY), 0, K_NO_WAIT);
	k_thread_name_set(&wdt_ctx.feed_thread, "watchdog_feed");
#endif

#ifdef CONFIG_HALO_PM_MANAGER
	/* Register PM callback for sleep/wake handling */
	int ret = halo_pm_register_callback(&wdt_ctx.pm_cb, watchdog_pm_callback, NULL,
				    "watchdog_pm", 0); /* Lowest priority in 0-100 scheme */
	if (ret < 0) {
		LOG_WRN("Failed to register PM callback: %d", ret);
	}
#endif

	LOG_DBG("Watchdog initialized with %u ms timeout", wdt_ctx.timeout_ms);

	return 0;
}

int halo_watchdog_feed(void)
{
	if (!wdt_ctx.initialized) {
		return -ENOTCONN;
	}

	int err = wdt_feed(wdt_ctx.wdt_dev, wdt_ctx.wdt_channel_id);
	if (err < 0) {
		LOG_ERR("Failed to feed watchdog: %d", err);
		return err;
	}

	return 0;
}

int halo_watchdog_set_timeout(uint32_t timeout_ms)
{
	/* Note: Changing timeout may not be supported by all hardware */
	/* This is a placeholder - actual implementation depends on hardware capabilities */
	LOG_WRN("Setting watchdog timeout not implemented for this hardware");
	return -ENOTSUP;
}

int halo_watchdog_get_timeout(uint32_t *timeout_ms)
{
	if (!timeout_ms) {
		return -EINVAL;
	}

	*timeout_ms = wdt_ctx.timeout_ms;
	return 0;
}

int halo_watchdog_register_callback(halo_watchdog_callback_t callback, void *user_data)
{
	if (!wdt_ctx.initialized) {
		return -ENOTCONN;
	}

#if !CONFIG_HALO_WATCHDOG_ALLOW_CALLBACK
	return -ENOTSUP;
#endif

	wdt_ctx.callback = callback;
	wdt_ctx.callback_user_data = user_data;

	return 0;
}

int halo_watchdog_get_status(int *fed, uint32_t *timeout_ms)
{
	if (!wdt_ctx.initialized) {
		return -ENOTCONN;
	}

	/* Note: Actual fed status depends on hardware/driver support */
	/* This is a simplified implementation */
	if (fed) {
		*fed = 1; /* Assume fed if initialized */
	}

	if (timeout_ms) {
		*timeout_ms = wdt_ctx.timeout_ms;
	}

	return 0;
}

int halo_watchdog_suspend(void)
{
	if (!wdt_ctx.initialized) {
		return -ENOTCONN;
	}

#ifdef CONFIG_HALO_WATCHDOG_AUTO_FEED
	k_thread_suspend(&wdt_ctx.feed_thread);
#endif

	int err = wdt_disable(wdt_ctx.wdt_dev);
	if (err < 0) {
		LOG_ERR("Failed to disable watchdog: %d", err);
		return err;
	}

	LOG_DBG("Watchdog disabled");
	return 0;
}

int halo_watchdog_resume(void)
{
	if (!wdt_ctx.initialized) {
		return -ENOTCONN;
	}

	int err = wdt_setup(wdt_ctx.wdt_dev, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (err < 0) {
		LOG_ERR("Failed to re-setup watchdog: %d", err);
		return err;
	}

#ifdef CONFIG_HALO_WATCHDOG_AUTO_FEED
	k_thread_resume(&wdt_ctx.feed_thread);
#endif

	LOG_DBG("Watchdog re-enabled");
	return 0;
}

bool halo_watchdog_has_fired(void)
{
	return watchdog_fired == HALO_WATCHDOG_FIRED_MAGIC;
}