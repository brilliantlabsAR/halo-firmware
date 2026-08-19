/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <stdio.h>
#include "storage.h"

LOG_MODULE_REGISTER(storage, CONFIG_STORAGE_LOG_LEVEL);

struct storage_ctx {
	uint8_t *p_output;
	size_t size;
};

static int settings_direct_loader(const char *const key, size_t const len,
				  settings_read_cb const read_cb, void *cb_arg, void *param)
{
	struct storage_ctx *p_ctx = (struct storage_ctx *)param;

	/* Handle the subtree if it is an exact key match. */
	if (settings_name_next(key, NULL) == 0) {
		ssize_t const cb_len = read_cb(cb_arg, p_ctx->p_output, p_ctx->size);

		if (cb_len != p_ctx->size) {
			LOG_ERR("Unable to read bytes_written from storage");
			return cb_len;
		}
	}

	return 0;
}

int storage_init(void)
{
	int err = settings_subsys_init();

	if (err) {
		LOG_ERR("settings_subsys_init() failed (err %d)", err);
		return err;
	}
	return 0;
}

int storage_save(const char *key, void *data, size_t const size)
{
	int err;
	char key_str[64];

	snprintf(key_str, sizeof(key_str), SETTINGS_BASE "/%s", key);
	err = settings_save_one(key_str, data, size);
	if (err) {
		LOG_ERR("Failed to store %s data (err %d)", key, err);
	}
	return err;
}

int storage_load(const char *key, void *data, size_t const size)
{
	struct storage_ctx ctx = {
		.p_output = data,
		.size = size,
	};
	char key_str[64];

	snprintf(key_str, sizeof(key_str), SETTINGS_BASE "/%s", key);
	return settings_load_subtree_direct(key_str, settings_direct_loader, &ctx);
}
