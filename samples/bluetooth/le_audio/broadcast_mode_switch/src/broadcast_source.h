/* Copyright Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef _BROADCAST_SOURCE_H
#define _BROADCAST_SOURCE_H

/**
 * @brief Configure the BAP BC source module
 *
 * This function should be called once during initialization.
 * Subsequent calls will be ignored if already configured.
 *
 * @retval 0 on success
 * @retval Negative error code on failure
 */
int broadcast_source_configure(void);

/**
 * @brief Start the LE audio broadcast source
 *
 * This function will configure the modules if not already done,
 * then start the broadcast source.
 *
 * @retval 0 on success
 * @retval Negative error code on failure
 */
int broadcast_source_start(void);

/**
 * @brief Start broadcasting (enable PA and start streaming)
 *
 * This function only starts broadcasting. The BAP BC source modules must
 * be configured first using broadcast_source_configure().
 *
 * @retval 0 on success
 * @retval Negative error code on failure
 */
int broadcast_source_start_broadcasting(void);

/**
 * @brief Stop the LE audio broadcast source
 *
 * @retval 0 on success
 * @retval Negative error code on failure
 */
int broadcast_source_stop(void);

#endif /* _BROADCAST_SOURCE_H */
