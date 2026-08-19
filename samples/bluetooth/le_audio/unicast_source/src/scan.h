/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

 #ifndef _SCAN_H
#define _SCAN_H

/**
 * @brief Callback function type for scanning ready event.
 */
typedef void (*scanning_ready_callback_t)(void);

/**
 * @brief Configure scanning for LE audio devices
 *
 * @return 0 on success
 */
int unicast_source_scan_configure(void);

/**
 * @brief Start scanning for LE audio devices
 *
 * @return 0 on success
 */
int unicast_source_scan_start(scanning_ready_callback_t ready_cb);

/**
 * @brief Stop scanning for LE audio devices
 *
 * @return 0 on success
 */
int unicast_source_scan_stop(void);

#endif /* _SCAN_H */
