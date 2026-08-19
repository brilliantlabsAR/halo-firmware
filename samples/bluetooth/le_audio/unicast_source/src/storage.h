/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef _STORAGE_H
#define _STORAGE_H

#include <stdint.h>

#define SETTINGS_BASE           "uc_central"
#define SETTINGS_NAME_KEYS      "bond_keys"
#define SETTINGS_NAME_BOND_DATA "bond_data"
#define SETTINGS_NAME_PEER      "peer"

/**
 * @brief Stores data in the flash memory.
 *
 * @param key Key string to identify the data.
 * @param data Pointer to the data to store.
 * @param size Size of the data.
 *
 * @return 0 on success, error code otherwise.
 */
int storage_init(void);

/**
 * @brief Stores data in the flash memory.
 *
 * @param key Key string to identify the data.
 * @param index Index of the data to store.
 * @param data Pointer to the data to store.
 * @param size Size of the data.
 *
 * @return 0 on success, error code otherwise.
 */
int storage_store(const char *key, int index, void *data, size_t size);

/**
 * @brief Loads data from the flash memory.
 *
 * @param key Key string to identify the data.
 * @param index Index of the data to load.
 * @param data Pointer to store the loaded data.
 * @param size Size of the data to load.
 *
 * @return 0 on success, error code otherwise.
 */
int storage_load(const char *key, int index, void *data, size_t size);

#endif /* _STORAGE_H */
