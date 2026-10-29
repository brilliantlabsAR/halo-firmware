/*
 * Copyright (c) 2025 Brilliant Labs Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Diagnostic log for bring-up experiments: see halo/diag_log.h.
 */

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <halo/diag_log.h>
#include <halo/file_manager.h>

#ifdef CONFIG_HALO_BATTERY_MANAGER
#include <halo/battery_manager.h>
#endif

LOG_MODULE_REGISTER(halo_diag, CONFIG_HALO_LOG_LEVEL);

#define DIAG_LINE_MAX  160
#define DIAG_EARLY_MAX 1024
#define DIAG_CFG_MAX   16
#define DIAG_KEY_MAX   24

static K_MUTEX_DEFINE(diag_lock);
static struct fs_file_t diag_file;
static bool diag_open;
static size_t diag_size;
static char diag_path[32];
static char diag_old_path[32];

/* Lines logged before halo_diag_init() */
static char early[DIAG_EARLY_MAX];
static size_t early_len;
static int early_dropped;

static struct {
	char key[DIAG_KEY_MAX];
	int value;
} cfg[DIAG_CFG_MAX];
static int cfg_count;

/* Heartbeat and battery watch run on their own queue: a battery reading
 * sleeps ~70 ms, which the system work queue should not have to wait for. */
static K_THREAD_STACK_DEFINE(diag_wq_stack, 1536);
static struct k_work_q diag_wq;
static bool diag_wq_started;

static struct k_work_delayable heartbeat_work;
static int heartbeat_s;

static struct k_work_delayable watch_work;
static volatile bool watch_on;
static int watch_period_ms;
static int watch_min;
static int watch_samples;

static int diag_file_open(void)
{
	struct fs_dirent st;

	fs_file_t_init(&diag_file);
	int ret = fs_open(&diag_file, diag_path, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND);
	if (ret < 0) {
		LOG_ERR("diag: open %s failed: %d", diag_path, ret);
		return ret;
	}
	diag_open = true;
	diag_size = (fs_stat(diag_path, &st) == 0) ? st.size : 0;
	return 0;
}

/* Start a new file once this one is full; the previous one becomes diag.old */
static void diag_rotate(void)
{
	fs_close(&diag_file);
	diag_open = false;
	(void)fs_unlink(diag_old_path);
	(void)fs_rename(diag_path, diag_old_path);
	(void)diag_file_open();
}

static void diag_write(const char *line, size_t len)
{
	if (!diag_open) {
		return;
	}
	if (diag_size + len > CONFIG_HALO_DIAG_LOG_MAX_BYTES) {
		diag_rotate();
		if (!diag_open) {
			return;
		}
	}
	ssize_t n = fs_write(&diag_file, line, len);
	if (n > 0) {
		diag_size += n;
	}
	(void)fs_sync(&diag_file);
}

static void cfg_parse(char *text)
{
	for (char *line = strtok(text, "\r\n"); line != NULL && cfg_count < DIAG_CFG_MAX;
	     line = strtok(NULL, "\r\n")) {
		char *eq = strchr(line, '=');

		if (line[0] == '#' || eq == NULL || eq - line >= DIAG_KEY_MAX) {
			continue;
		}
		*eq = '\0';
		strncpy(cfg[cfg_count].key, line, DIAG_KEY_MAX - 1);
		cfg[cfg_count].value = atoi(eq + 1);
		cfg_count++;
	}
}

static void cfg_read(void)
{
	char path[32];
	char text[256];
	struct fs_file_t f;
	struct fs_dirent st;

	snprintf(path, sizeof(path), "%s/diag.cfg", halo_file_mount_point());
	if (fs_stat(path, &st) != 0) {
		return; /* no settings: the defaults (fs_open would log an error) */
	}
	fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_READ) < 0) {
		return;
	}
	ssize_t n = fs_read(&f, text, sizeof(text) - 1);
	fs_close(&f);
	if (n <= 0) {
		return;
	}
	text[n] = '\0';
	cfg_parse(text);
}

int halo_diag_setting(const char *key, int def)
{
	for (int i = 0; i < cfg_count; i++) {
		if (strcmp(cfg[i].key, key) == 0) {
			return cfg[i].value;
		}
	}
	return def;
}

int halo_diag_init(void)
{
	snprintf(diag_path, sizeof(diag_path), "%s/diag.txt", halo_file_mount_point());
	snprintf(diag_old_path, sizeof(diag_old_path), "%s/diag.old", halo_file_mount_point());
	cfg_read();

	k_mutex_lock(&diag_lock, K_FOREVER);
	int ret = diag_file_open();
	if (ret == 0) {
		diag_write(early, early_len);
		if (early_dropped) {
			char line[48];
			int len = snprintf(line, sizeof(line), "(%d early lines dropped)\n",
					   early_dropped);
			diag_write(line, len);
		}
	}
	early_len = 0;
	k_mutex_unlock(&diag_lock);
	return ret;
}

void halo_diag_log(const char *fmt, ...)
{
	char line[DIAG_LINE_MAX];
	va_list ap;
	int64_t now = k_uptime_get();
	int len = snprintf(line, sizeof(line), "[%7lld] ", now);

	va_start(ap, fmt);
	len += vsnprintf(line + len, sizeof(line) - len - 1, fmt, ap);
	va_end(ap);
	if (len > (int)sizeof(line) - 2) {
		len = sizeof(line) - 2;
	}
	LOG_INF("%s", line);
	line[len++] = '\n';

	k_mutex_lock(&diag_lock, K_FOREVER);
	if (diag_open) {
		diag_write(line, len);
	} else if (early_len + len <= sizeof(early)) {
		memcpy(early + early_len, line, len);
		early_len += len;
	} else {
		early_dropped++;
	}
	k_mutex_unlock(&diag_lock);
}

int halo_diag_vbat_mv(void)
{
#ifdef CONFIG_HALO_BATTERY_MANAGER
	if (halo_battery_update_now() < 0) {
		return -1;
	}
	return halo_battery_get_voltage();
#else
	return -1;
#endif
}

int halo_diag_vbat_cached_mv(void)
{
#ifdef CONFIG_HALO_BATTERY_MANAGER
	int mv = halo_battery_get_voltage();
	return mv < 0 ? -1 : mv;
#else
	return -1;
#endif
}

static void diag_wq_start(void)
{
	if (diag_wq_started) {
		return;
	}
	k_work_queue_start(&diag_wq, diag_wq_stack, K_THREAD_STACK_SIZEOF(diag_wq_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);
	k_thread_name_set(&diag_wq.thread, "diag_wq");
	diag_wq_started = true;
}

static void heartbeat_fn(struct k_work *work)
{
	ARG_UNUSED(work);
#ifdef CONFIG_HALO_BATTERY_MANAGER
	halo_diag_log("alive vbat=%d charging=%d", halo_diag_vbat_cached_mv(),
		      halo_battery_is_charging());
#else
	halo_diag_log("alive");
#endif
	if (heartbeat_s > 0) {
		k_work_schedule_for_queue(&diag_wq, &heartbeat_work, K_SECONDS(heartbeat_s));
	}
}

void halo_diag_heartbeat(int seconds)
{
	static bool initialized;

	diag_wq_start();
	if (!initialized) {
		k_work_init_delayable(&heartbeat_work, heartbeat_fn);
		initialized = true;
	}
	heartbeat_s = seconds;
	if (seconds > 0) {
		k_work_schedule_for_queue(&diag_wq, &heartbeat_work, K_SECONDS(seconds));
	} else {
		k_work_cancel_delayable(&heartbeat_work);
	}
}

static void watch_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (!watch_on) {
		return;
	}
	int mv = halo_diag_vbat_mv();

	if (mv > 0) {
		if (watch_samples == 0 || mv < watch_min) {
			watch_min = mv;
		}
		watch_samples++;
	}
	if (watch_on) {
		k_work_schedule_for_queue(&diag_wq, &watch_work, K_MSEC(watch_period_ms));
	}
}

void halo_diag_vbat_watch_start(int period_ms)
{
	static bool initialized;

	diag_wq_start();
	if (!initialized) {
		k_work_init_delayable(&watch_work, watch_fn);
		initialized = true;
	}
	watch_min = -1;
	watch_samples = 0;
	watch_period_ms = period_ms;
	watch_on = true;
	k_work_schedule_for_queue(&diag_wq, &watch_work, K_NO_WAIT);
}

int halo_diag_vbat_watch_stop(int *samples)
{
	struct k_work_sync sync;

	watch_on = false;
	k_work_cancel_delayable_sync(&watch_work, &sync);
	if (samples) {
		*samples = watch_samples;
	}
	return watch_samples ? watch_min : -1;
}
