/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_FILE_MANAGER_H
#define HALO_FILE_MANAGER_H

#include <zephyr/kernel.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the file system
 *
 * Mounts LittleFS and initializes settings subsystem.
 * If mount fails, will erase and reformat the flash.
 *
 * @return 0 on success, negative error code on failure
 */
int halo_file_init(void);

/**
 * @brief Deinitialize the file system
 *
 * Unmounts the file system.
 *
 * @return 0 on success, negative error code on failure
 */
int halo_file_deinit(void);

/**
 * @brief Get the mount point path
 *
 * @return Pointer to mount point string (e.g., "/lfs")
 */
const char *halo_file_mount_point(void);

/**
 * @brief Format the file system
 *
 * Erases all data and remounts the file system.
 *
 * @return 0 on success, negative error code on failure
 */
int halo_file_format(void);

/**
 * @brief Remove all files and folders (except settings)
 *
 * Deletes all files and directories in the file system, except
 * for the settings file which contains pairing information and
 * configuration settings.
 *
 * @return 0 on success, negative error code on failure
 */
int halo_file_remove_all(void);

/**
 * @brief Get a setting value
 *
 * @param key Setting key
 * @param data Buffer to store the value
 * @param size Size of the buffer
 * @return 0 on success, negative error code on failure
 */
int halo_settings_get(const char *key, void *data, size_t size);

/**
 * @brief Set a setting value
 *
 * @param key Setting key
 * @param data Value to store
 * @param size Size of the value
 * @return 0 on success, negative error code on failure
 */
int halo_settings_set(const char *key, const void *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* HALO_FILE_MANAGER_H */
