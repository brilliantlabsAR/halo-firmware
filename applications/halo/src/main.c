/*
 * Copyright (c) 2025 Brilliant Labs Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/drivers/sensor.h>
#include <app_version.h>

#include <halo/mem_manager.h>
#include <halo/file_manager.h>
#include <halo/pm_manager.h>
#include <halo/led_manager.h>

#include <zephyr/drivers/input/button.h>

#ifdef CONFIG_SENSOR
#include <halo/battery_manager.h>
#endif

#ifdef CONFIG_HALO_BLE_MANAGER
#include <halo/ble_manager.h>
#include <halo/ble_connection.h>
#include <halo/ble_security.h>
#ifdef CONFIG_HALO_BLE_LUA_SERVICE
#include <halo/ble_lua.h>
#endif
#endif

#ifdef CONFIG_HALO_LUA_RUNTIME
#include <halo/lua_runtime.h>
#endif

#if defined(CONFIG_HALO_STARTUP_SOUND) || defined(CONFIG_HALO_SHUTDOWN_SOUND)
#include <halo/sfxr.h>
#endif

#ifdef CONFIG_HALO_BOOT_LOGO
#include <halo/boot_logo.h>
#endif

#ifdef CONFIG_HALO_WATCHDOG_MANAGER
#include <halo/watchdog_manager.h>
#endif

#include <zephyr/drivers/regulator.h>
#include <zephyr/pm/device.h>

#if defined(CONFIG_BOOTLOADER_MCUBOOT) && defined(CONFIG_MCUBOOT_IMG_MANAGER)
#include <zephyr/dfu/mcuboot.h>
#endif

#include "se_service.h"

LOG_MODULE_REGISTER(halo, CONFIG_HALO_LOG_LEVEL);

const char halo_banner[] = "    _    _       _       \n"
			   "   | |  | |     | |      \n"
			   "   | |__| | __ _| | ___  \n"
			   "   |  __  |/ _` | |/ _ \\ \n"
			   "   | |  | | (_| | | (_) |\n"
			   "   |_|  |_|\\__,_|_|\\___/ \n"
			   "   Copyright (C) 2025 Brilliant Labs\n"
			   "   https://brilliant.xyz/\n";

/**
 * @brief Main application entry point
 *
 * Initializes all core subsystems and starts the Lua runtime.
 * The system is then driven by Lua scripts and BLE events.
 */
int main(void)
{
	int ret;

#ifdef CONFIG_HALO_WATCHDOG_MANAGER
	if (halo_watchdog_has_fired()) {
		// Reboot on watchdog timeout, make sure everything is fresh
#ifdef CONFIG_HALO_BLE_MANAGER
		halo_ble_conn_prepare_reboot();
#endif
		sys_reboot(SYS_REBOOT_COLD);
	}
#endif
#ifdef CONFIG_HALO_LOG_LEVEL_DBG
	printk("\n%s", halo_banner);
	printk("Hardware Version: %s\n", CONFIG_BOARD);
	printk("Firmware Version: %s\n", APP_VERSION_STRING);
	printk("Build Date: %s\n", __DATE__);
#endif

#ifdef CONFIG_HALO_PM_MANAGER
	/* Initialize power manager */
	ret = halo_pm_init();
	if (ret < 0) {
		__ASSERT(false, "Failed to initialize power manager: %d", ret);
		return ret;
	}
#endif

#ifdef CONFIG_HALO_MEM_MANAGER
	/* Initialize memory manager */
	ret = halo_mem_init();
	if (ret < 0) {
		__ASSERT(false, "Failed to initialize memory manager: %d", ret);
		return ret;
	}
#endif

#ifdef CONFIG_HALO_WATCHDOG_MANAGER
	/* Initialize watchdog manager */
	ret = halo_watchdog_init();
	if (ret < 0) {
		__ASSERT(false, "Failed to initialize watchdog manager: %d", ret);
		return ret;
	}
#endif

#ifdef CONFIG_HALO_LED_MANAGER
	/* Initialize LED manager */
	ret = halo_led_init();
	if (ret < 0) {
		__ASSERT(false, "Failed to initialize LED manager: %d", ret);
		return ret;
	}
#endif

#ifdef CONFIG_HALO_BATTERY_MANAGER
	/* Initialize battery manager */
	ret = halo_battery_init();
	if (ret < 0) {
		__ASSERT(false, "Failed to initialize battery manager: %d", ret);
		return ret;
	}
#endif

#ifdef CONFIG_HALO_FILE_MANAGER
	/* Initialize file manager */
	ret = halo_file_init();
	if (ret < 0) {
		__ASSERT(false, "Failed to initialize file manager: %d", ret);
		return ret;
	}
#endif

#ifdef CONFIG_HALO_BLE_MANAGER
	/* Initialize BLE manager */
	ret = halo_ble_init(NULL);
	if (ret < 0) {
		__ASSERT(false, "Failed to initialize BLE manager: %d", ret);
		return ret;
	}

#endif /* CONFIG_HALO_BLE_MANAGER */

#if defined(CONFIG_HALO_STARTUP_SOUND) || defined(CONFIG_HALO_BOOT_LOGO)
	/*
	 * Boot splash: kick off the (async) startup sound and draw the logo
	 * BEFORE the Lua runtime starts. main.lua runs on the REPL thread as
	 * soon as halo_lua_runtime_init() is called and would clear the display
	 * out from under the logo. Holding here on the main thread (which only
	 * sleeps forever afterwards) keeps the splash up with no race against
	 * the app: we wait for the sound to finish (when enabled), then top up
	 * to a minimum hold time. An unpaired device holds longer and shows its
	 * BLE name under the logo so the user can identify it while pairing.
	 */
#ifdef CONFIG_HALO_STARTUP_SOUND
	halo_startup_sound_schedule();
#endif
#ifdef CONFIG_HALO_BOOT_LOGO
	bool splash_unpaired = false;
	const char *splash_label = NULL;
#ifdef CONFIG_HALO_BLE_MANAGER
	splash_unpaired = !halo_ble_sec_is_bonded();
	if (splash_unpaired) {
		splash_label = halo_ble_conn_get_device_name();
	}
#endif
	int64_t splash_start = k_uptime_get();

	if (halo_display_boot_logo_show(splash_label) == 0) {
#ifdef CONFIG_HALO_STARTUP_SOUND
		/* Bounded so a missing/stuck speaker can't stall boot. */
		halo_startup_sound_wait(CONFIG_HALO_STARTUP_SOUND_TIMEOUT_MS +
					CONFIG_HALO_STARTUP_SOUND_DURATION_MS + 250);
#endif
		int64_t hold_ms = splash_unpaired ? CONFIG_HALO_BOOT_LOGO_UNPAIRED_HOLD_MS
						  : CONFIG_HALO_BOOT_LOGO_MIN_HOLD_MS;
		int64_t remaining_ms = hold_ms - (k_uptime_get() - splash_start);

		if (remaining_ms > 0) {
			k_sleep(K_MSEC(remaining_ms));
		}
		halo_display_boot_logo_hide();
	}
#endif
#endif

#ifdef CONFIG_HALO_LUA_RUNTIME
	/* Initialize Lua runtime - this is the core of the system */
	ret = halo_lua_runtime_init();
	if (ret < 0) {
		__ASSERT(false, "Failed to initialize Lua runtime: %d", ret);
		return ret;
	}
#endif

#if defined(CONFIG_BOOTLOADER_MCUBOOT) && defined(CONFIG_MCUBOOT_IMG_MANAGER)
	/*
	 * Mark this image as confirmed now that the full boot sequence has
	 * succeeded. This must be the last step of bring-up: if any subsystem
	 * above hangs or faults, the image stays unconfirmed and MCUboot reverts
	 * to the previously good image on the next reset (the app-level watchdog
	 * provides that reset). Confirming earlier would defeat that safety net.
	 * boot_write_img_confirmed() is a no-op when the image is already confirmed.
	 */
	ret = boot_write_img_confirmed();
	if (ret < 0) {
		LOG_ERR("Failed to confirm MCUboot image: %d", ret);
	} else {
		LOG_INF("MCUboot image confirmed");
	}
#endif

#if defined(CONFIG_HALO_STARTUP_SOUND) || defined(CONFIG_HALO_SHUTDOWN_SOUND)
	/* Boot settled: re-arm the boot/shutdown cues for the next power-on.
	 * Must stay after the image confirm so a boot that ends earlier keeps
	 * the cue gate flag set (see CONFIG_HALO_CUE_GATE_DIRTY_BOOT). */
	halo_sound_cue_boot_settled();
#endif

	while (1) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
