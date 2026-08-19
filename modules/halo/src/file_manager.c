/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_backend_fs.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>

#include "halo/file_manager.h"

LOG_MODULE_REGISTER(halo_file, CONFIG_HALO_LOG_LEVEL);

#define STORAGE_PARTITION    storage_partition
#define STORAGE_PARTITION_ID FIXED_PARTITION_ID(STORAGE_PARTITION)

static struct {
	bool initialized;
} file_ctx;

/* Settings context for direct loader */
struct settings_ctx {
	uint8_t *p_output;
	size_t size;
};

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(cstorage);

/* Mount info */
static struct fs_mount_t littlefs_mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &cstorage,
	.storage_dev = (void *)STORAGE_PARTITION_ID,
	.mnt_point = "/lfs"
};

/* Settings direct loader callback */
static int settings_direct_loader(const char *key, size_t len,
				  settings_read_cb read_cb, void *cb_arg, void *param)
{
	struct settings_ctx *ctx = (struct settings_ctx *)param;

	/* Handle exact key match */
	if (settings_name_next(key, NULL) == 0) {
		ssize_t cb_len = read_cb(cb_arg, ctx->p_output, ctx->size);

		if (cb_len < 0) {
			/* Actual read error */
			LOG_ERR("Failed to read setting: error %d", cb_len);
			return cb_len;
		}
		
		if (cb_len > ctx->size) {
			/* Stored data is larger than buffer - error */
			LOG_ERR("Setting too large: stored %d, buffer %u", 
				cb_len, ctx->size);
			return -ENOMEM;
		}
		
		if (cb_len < ctx->size) {
			/* Stored data is smaller than requested - this is OK for strings */
		}
	}

	return -ENOENT;
}

/* Erase flash partition */
static int flash_erase_partition(unsigned int id)
{
	const struct flash_area *pfa;
	int ret;

	ret = flash_area_open(id, &pfa);
	if (ret < 0) {
		LOG_ERR("Failed to open flash area %u: %d", id, ret);
		return ret;
	}

	ret = flash_area_erase(pfa, 0, pfa->fa_size);
	if (ret < 0) {
		LOG_ERR("Failed to erase flash area: %d", ret);
	}

	flash_area_close(pfa);
	return ret;
}

int halo_file_init(void)
{
	int ret;

	if (file_ctx.initialized) {
		return 0;
	}

	/* Try to mount file system */
	ret = fs_mount(&littlefs_mnt);
	if (ret != 0) {
		LOG_WRN("Mount failed (%d), erasing and reformatting...", ret);
		
		/* Erase flash and retry */
		ret = flash_erase_partition((uintptr_t)littlefs_mnt.storage_dev);
		if (ret < 0) {
			LOG_ERR("Failed to erase partition: %d", ret);
			return ret;
		}

		ret = fs_mount(&littlefs_mnt);
		if (ret != 0) {
			LOG_ERR("Mount failed after erase: %d", ret);
			return ret;
		}
	}

	/* Create settings file if it doesn't exist */
	if (fs_stat(CONFIG_SETTINGS_FILE_PATH, NULL) != 0) {
		struct fs_file_t file;
		fs_file_t_init(&file);
		
		ret = fs_open(&file, CONFIG_SETTINGS_FILE_PATH, FS_O_CREATE | FS_O_RDWR);
		if (ret < 0) {
			LOG_ERR("Failed to create settings file: %d", ret);
			return ret;
		}
		fs_close(&file);
	}

	/* Initialize settings subsystem */
	ret = settings_subsys_init();
	if (ret != 0) {
		LOG_ERR("Settings subsystem init failed: %d", ret);
		return ret;
	}
	file_ctx.initialized = true;
	LOG_DBG("File manager initialized");
	return 0;
}

int halo_file_deinit(void)
{
	int ret;

	if (!file_ctx.initialized) {
		return 0;
	}

	ret = fs_unmount(&littlefs_mnt);
	if (ret != 0) {
		LOG_ERR("Unmount failed: %d", ret);
		return ret;
	}

	return 0;
}

const char *halo_file_mount_point(void)
{
	return littlefs_mnt.mnt_point;
}

int halo_file_format(void)
{
	int ret;

	/* Unmount first */
	ret = halo_file_deinit();
	if (ret < 0) {
		LOG_ERR("Failed to unmount before format: %d", ret);
		return ret;
	}

	/* Erase flash */
	ret = flash_erase_partition((uintptr_t)littlefs_mnt.storage_dev);
	if (ret < 0) {
		LOG_ERR("Failed to erase partition: %d", ret);
		return ret;
	}

	/* Remount */
	ret = fs_mount(&littlefs_mnt);
	if (ret != 0) {
		LOG_ERR("Mount failed after format: %d", ret);
		return ret;
	}

	return 0;
}

int halo_settings_get(const char *key, void *data, size_t size)
{
	struct settings_ctx ctx = {
		.p_output = data,
		.size = size,
	};

	return settings_load_subtree_direct(key, settings_direct_loader, &ctx);
}

int halo_settings_set(const char *key, const void *data, size_t size)
{
	return settings_save_one(key, data, size);
}

/* Recursive delete with depth limit to avoid stack overflow */
static int delete_directory_contents(const char *path, int depth)
{
	/* Safety checks */
	if (path == NULL || path[0] == '\0') {
		return -EINVAL;
	}

	if (depth > 8) {
		/* Too deep, skip to avoid stack overflow */
		LOG_WRN("Directory too deep, skipping: %s", path);
		return -1;
	}

	struct fs_dir_t dir;
	fs_dir_t_init(&dir);

	int ret = fs_opendir(&dir, path);
	if (ret < 0) {
		/* Directory might not exist or already deleted */
		return (ret == -ENOENT) ? 0 : ret;
	}

	/* Collect entries first */
	typedef struct {
		char name[64];
		uint8_t type;
	} entry_t;

	entry_t entries[32];
	int entry_count = 0;

	struct fs_dirent entry;
	while (entry_count < 32) {
		ret = fs_readdir(&dir, &entry);
		if (ret < 0) {
			/* Error reading directory, stop but continue */
			break;
		}

		if (entry.name[0] == '\0') {
			/* End of directory */
			break;
		}

		/* Skip special entries */
		if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0) {
			continue;
		}

		/* Safety: ensure name is not too long */
		if (strlen(entry.name) >= sizeof(entries[entry_count].name)) {
			LOG_WRN("Entry name too long, skipping");
			continue;
		}

		strncpy(entries[entry_count].name, entry.name, sizeof(entries[entry_count].name) - 1);
		entries[entry_count].name[sizeof(entries[entry_count].name) - 1] = '\0';
		entries[entry_count].type = entry.type;
		entry_count++;
	}

	/* Always close directory, ignore errors */
	fs_closedir(&dir);

	/* Delete all entries with error tolerance */
	char full_path[256];
	for (int i = 0; i < entry_count; i++) {
		/* Build path safely */
		ret = snprintf(full_path, sizeof(full_path), "%s/%s", path, entries[i].name);
		if (ret < 0 || ret >= sizeof(full_path)) {
			LOG_WRN("Path too long, skipping: %s/%s", path, entries[i].name);
			continue;
		}

		if (entries[i].type == FS_DIR_ENTRY_DIR) {
			/* Recursively delete subdirectory - ignore errors */
			delete_directory_contents(full_path, depth + 1);
			/* Try to delete the directory - ignore errors */
			ret = fs_unlink(full_path);
			if (ret < 0 && ret != -ENOENT && ret != -ENOTEMPTY) {
				/* Silent fail - might have subdirectories we couldn't process */
			}
		} else {
			/* Delete file - ignore errors */
			ret = fs_unlink(full_path);
			if (ret < 0 && ret != -ENOENT) {
				/* Silent fail - file might be in use */
			}
		}
	}

	return 0;
}

int halo_file_remove_all(void)
{
	const char *mount_point = halo_file_mount_point();

	/* Safety check */
	if (mount_point == NULL || mount_point[0] == '\0') {
		LOG_ERR("Invalid mount point");
		return -EINVAL;
	}

	struct fs_dir_t dir;
	fs_dir_t_init(&dir);

	int ret = fs_opendir(&dir, mount_point);
	if (ret < 0) {
		LOG_ERR("Failed to open mount point: %d", ret);
		return ret;
	}

	/* Collect root level entries */
	typedef struct {
		char name[64];
		uint8_t type;
	} entry_t;

	entry_t entries[32];
	int entry_count = 0;

	struct fs_dirent entry;
	while (entry_count < 32) {
		ret = fs_readdir(&dir, &entry);
		if (ret < 0) {
			/* Error reading, stop but continue */
			break;
		}

		if (entry.name[0] == '\0') {
			/* End of directory */
			break;
		}

		/* Skip special entries */
		if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0) {
			continue;
		}

		/* Preserve settings file */
		if (strcmp(entry.name, "settings") == 0) {
			continue;
		}

		/* Safety: ensure name is not too long */
		if (strlen(entry.name) >= sizeof(entries[entry_count].name)) {
			LOG_WRN("Entry name too long, skipping");
			continue;
		}

		strncpy(entries[entry_count].name, entry.name, sizeof(entries[entry_count].name) - 1);
		entries[entry_count].name[sizeof(entries[entry_count].name) - 1] = '\0';
		entries[entry_count].type = entry.type;
		entry_count++;
	}

	/* Always close directory, ignore errors */
	fs_closedir(&dir);

	/* Delete all root level entries with error tolerance */
	char full_path[256];
	for (int i = 0; i < entry_count; i++) {
		/* Build path safely */
		ret = snprintf(full_path, sizeof(full_path), "%s/%s", mount_point, entries[i].name);
		if (ret < 0 || ret >= sizeof(full_path)) {
			LOG_WRN("Path too long, skipping: %s/%s", mount_point, entries[i].name);
			continue;
		}

		if (entries[i].type == FS_DIR_ENTRY_DIR) {
			/* Recursively delete directory contents - ignore errors */
			delete_directory_contents(full_path, 0);
			/* Try to delete the directory - ignore errors */
			ret = fs_unlink(full_path);
			if (ret < 0 && ret != -ENOENT && ret != -ENOTEMPTY) {
				/* Silent fail - might have subdirectories we couldn't process */
			}
		} else {
			/* Delete file - ignore errors */
			ret = fs_unlink(full_path);
			if (ret < 0 && ret != -ENOENT) {
				/* Silent fail - file might be in use */
			}
		}
	}

	/* Reset the LOG_BACKEND_FS backend so it releases its handle to the
	 * deleted log files and re-initializes on the next log write (same as
	 * frame.log.clear). Without this its rotation counters go stale and
	 * del_oldest_log() scans all 10000 file numerals forever, each failed
	 * unlink logging an error — a storm that starves lower-priority
	 * threads (frozen app) and the idle thread (blocked power-off). */
	log_backend_fs_reset();

	LOG_INF("All user files removed (settings preserved)");
	return 0;
}
