/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef ALIF_ADDRESS_VERIFICATION_H_
#define ALIF_ADDRESS_VERIFICATION_H_

#include "gapm.h"

enum alif_addr_type {
	/*Generated static random address*/
	ALIF_STATIC_RAND_ADDR = 0u,
	/*Generated resolvable private random address*/
	ALIF_GEN_RSLV_RAND_ADDR,
	/*Generated non-resolvable private random address*/
	ALIF_GEN_NON_RSLV_RAND_ADDR,
	/*Generated Public Address*/
	ALIF_PUBLIC_ADDR,
};
/**
 * @brief Verifies the address type and sets the appropriate fields in the gapm_config_t structure.
 *
 * @param addr_type address type to be verified.
 * @param adv_type Pointer to the advertising type to be set based on the address type.
 * @param gapm_cfg Pointer to the gapm_config_t structure to be updated
 * with the address configuration.
 * @return 0 on success, negative error code on failure.
 */
uint8_t address_verification(uint8_t addr_type, uint8_t *adv_type, gapm_config_t *gapm_cfg);
/**
 * @brief Logs device identity information.
 */
void print_device_identity(void);

/**
 * @brief Logs device's advertising address.
 *
 * @param actv_idx Active index connection from which the address will be fetched.
 */
void address_verification_log_advertising_address(const uint8_t actv_idx);

#endif /* ALIF_ADDRESS_VERIFICATION_H_ */
