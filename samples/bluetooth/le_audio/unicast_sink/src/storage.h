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

#define SETTINGS_BASE           "uc_periph"
#define SETTINGS_NAME_KEYS      "bond_keys_0"
#define SETTINGS_NAME_BOND_DATA "bond_data_0"
#define SETTINGS_NAME_VOLUME    "volume"
#define SETTINGS_NAME_PEER      "peer"
#define SETTINGS_NAME_ADDR      "address"

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
 * @param data Pointer to the data to store.
 * @param size Size of the data.
 *
 * @return 0 on success, error code otherwise.
 */
int storage_save(const char *key, void *data, size_t size);

/**
 * @brief Loads data from the flash memory.
 *
 * @param key Key string to identify the data.
 * @param data Pointer to store the loaded data.
 * @param size Size of the data to load.
 *
 * @return 0 on success, error code otherwise.
 */
int storage_load(const char *key, void *data, size_t size);

#endif /* _STORAGE_H */
