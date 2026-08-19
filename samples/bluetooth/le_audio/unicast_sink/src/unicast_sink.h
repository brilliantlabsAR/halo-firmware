/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef _UNICAST_SINK_H
#define _UNICAST_SINK_H

/**
 * @brief Configure LE audio unicast sink functionality
 *
 * @return 0 on success
 */
int unicast_sink_init(void);

/**
 * @brief Start the LE audio unicast advertising
 *
 * @param p_address Pointer to the client device address for directed advertising
 *
 * @return 0 on success
 */
int unicast_sink_adv_start(void const * const p_address);

/**
 * @brief Stop the advertising
 *
 * @return 0 on success
 */
int unicast_sink_adv_stop(void);

#endif /* _UNICAST_SINK_H */
