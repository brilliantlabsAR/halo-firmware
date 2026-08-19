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
 * @brief Initialize BLE services for broadcast sink (BASS, scan, sink)
 *
 * @retval 0 on success
 * @retval Negative error code on failure
 */
int broadcast_sink_init(void);

/**
 * @brief Start BASS solicitation advertising with a consistent Extended Adv payload.
 *
 * The payload includes Complete Local Name (if it fits) and Appearance.
 *
 * @param device_name  Device name.
 * @param appearance   GAP Appearance value.
 * @retval 0 on success
 * @retval Negative error code on failure
 */
int broadcast_sink_start_solicitation(const char *device_name, uint16_t appearance);

#endif /* _BROADCAST_SINK_H */
