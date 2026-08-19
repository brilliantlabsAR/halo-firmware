/**
 * @brief Bluetooth scan response data manipulation API
 *
 * Copyright Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */
#ifndef BT_SCAN_RSP_H_
#define BT_SCAN_RSP_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize scan response data module
 *
 * @return 0 on success, negative error code otherwise
 */
int bt_scan_rsp_init(void);

/**
 * @brief Set empty scan response data
 *
 * @param actv_idx Activity index for the advertising set
 * @return 0 on success, negative error code otherwise
 */
int bt_scan_rsp_set(uint8_t actv_idx);

/**
 * @brief Set name in scan response data
 *
 * @param actv_idx Activity index for the advertising set
 * @param name Device name to set
 * @param name_len Length of the device name
 * @return 0 on success, negative error code otherwise
 */
int bt_scan_rsp_set_name(uint8_t actv_idx, const char *name, size_t name_len);

#ifdef __cplusplus
}
#endif

#endif /* BT_SCAN_RSP_H_ */
