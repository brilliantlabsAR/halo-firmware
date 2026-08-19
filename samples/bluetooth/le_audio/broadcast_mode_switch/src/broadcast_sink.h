/* Copyright Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef _BROADCAST_SINK_H
#define _BROADCAST_SINK_H

/**
 * @brief Configure the BAP BC modules (scan and sink)
 *
 * This function should be called once during initialization.
 * Subsequent calls will be ignored if already configured.
 *
 * @retval 0 on success
 * @retval Negative error code on failure
 */
int broadcast_sink_configure(void);

/**
 * @brief Start the LE audio broadcast sink
 *
 * This function will configure the modules if not already done,
 * then start scanning for broadcast sources.
 *
 * @retval 0 on success
 * @retval Negative error code on failure
 */
int broadcast_sink_start(void);

/**
 * @brief Start scanning for broadcast sources
 *
 * This function only starts scanning. The BAP BC modules must
 * be configured first using broadcast_sink_configure().
 *
 * @retval 0 on success
 * @retval Negative error code on failure
 */
int broadcast_sink_start_scanning(void);

/**
 * @brief Stop the LE audio broadcast sink
 *
 * @retval 0 on success
 * @retval Negative error code on failure
 */
int broadcast_sink_stop(void);

#endif /* _BROADCAST_SINK_H */
