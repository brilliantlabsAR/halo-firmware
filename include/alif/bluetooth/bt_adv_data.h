/**
 * @brief Bluetooth advertising data manipulation API
 *
 * Copyright Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */
#ifndef BT_ADV_DATA_H_
#define BT_ADV_DATA_H_

#include <stddef.h>
#include <stdint.h>
#include <gapm_le_adv.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize advertising data module
 *
 * @return 0 on success, negative error code otherwise
 */
int bt_adv_data_init(void);

/**
 * @brief Set service data in advertising data
 *
 * @param actv_idx Activity index for the advertising set
 * @param service_uuid Service UUID
 * @param data Service data
 * @param data_len Length of the service data
 * @return 0 on success, negative error code otherwise
 */
int bt_adv_data_set_service_data(uint8_t actv_idx, uint16_t service_uuid,
				const uint8_t *data, size_t data_len);

/**
 * @brief Clear all advertising data
 *
 * @param actv_idx Activity index for the advertising set
 * @return 0 on success, negative error code otherwise
 */
int bt_adv_data_clear(uint8_t actv_idx);

/**
 * @brief Get current advertising data length
 *
 * @return Current advertising data length in bytes
 */
uint8_t bt_adv_data_get_length(void);

/**
 * @brief Get pointer to raw advertising data
 *
 * @return Pointer to raw advertising data, or NULL if not available
 */
uint8_t *bt_adv_data_get_raw(void);

/**
 * @brief Check if advertising data contains a name
 *
 * @param name Buffer to store the name if found
 * @param max_len Maximum length of the name buffer
 * @return Length of the name on success, negative error code otherwise
 */
int bt_adv_data_check_name(char *name, size_t max_len);

/**
 * @brief Get the name from advertising data (complete or shortened)
 *
 * @param name Buffer to store the device name
 * @param max_len Maximum length of the buffer
 * @return Length of the name on success, negative error code otherwise
 */
int bt_adv_data_get_name_auto(char *name, size_t max_len);

/**
 * @brief Set device name in advertising data, automatically using shortened name if needed
 *
 * This function automatically determines whether to use a complete or shortened name
 * based on the available space in the advertising data. If the complete name doesn't fit,
 * it will be truncated and set as a shortened name.
 *
 * @param actv_idx Activity index for the advertising set
 * @param name Device name to set
 * @param name_len Length of the device name
 * @return 0 on success, negative error code otherwise
 */
int bt_adv_data_set_name_auto(uint8_t actv_idx, const char *name, size_t name_len);

/**
 * @brief Set manufacturer data in advertising data
 *
 * @param actv_idx Activity index for the advertising set
 * @param company_id Company identifier
 * @param data Manufacturer specific data
 * @param data_len Length of the manufacturer data
 * @return 0 on success, negative error code otherwise
 */
int bt_adv_data_set_manufacturer(uint8_t actv_idx, uint16_t company_id,
				const uint8_t *data, size_t data_len);


/**
 * @brief Set default advertising data for an activity
 *
 * @param actv_idx Activity index
 * @param device_name Pointer to device name string
 * @param name_len Length of the device name
 * @return 0 on success, negative errno otherwise
 */
int bt_adv_data_set_default(uint8_t actv_idx, const char *device_name, size_t name_len);

#ifdef __cplusplus
}
#endif

#endif /* BT_ADV_DATA_H_ */
