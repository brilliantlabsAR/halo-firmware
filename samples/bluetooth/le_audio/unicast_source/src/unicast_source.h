/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef _UNICAST_SOURCE_H
#define _UNICAST_SOURCE_H

#include <stdint.h>

/**
 * @brief Configure the LE audio unicast source
 *
 * @return 0 on success
 */
int unicast_source_configure(void);

/**
 * @brief Start the service discovery
 *
 * @param con_lid Connection index
 *
 * @return 0 on success
 */
int unicast_source_discover(uint8_t con_lid);

/**
 * @brief Setup the streams for the LE audio unicast source
 *
 * @param con_lid Connection index
 *
 * @return 0 on success
 */
int unicast_setup_streams(uint8_t con_lid);

/**
 * @brief Enable the streams for the LE audio unicast source
 *
 * @param con_lid Connection index
 *
 * @return 0 on success
 */
int unicast_enable_streams(uint8_t con_lid);

#endif /* _UNICAST_SOURCE_H */
