/* Copyright Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef MODE_SWITCH_SERVICE_H_
#define MODE_SWITCH_SERVICE_H_

#include <zephyr/types.h>

/* Mode definitions */
typedef enum {
	MODE_SWITCH_IDLE,    /* Device in idle mode - not active */
	MODE_SWITCH_SINK,    /* Device in broadcast sink mode */
	MODE_SWITCH_SOURCE,  /* Device in broadcast source mode */

	MODE_SWITCH_MAX
} mode_switch_type_t;

/**
 * @brief Start the mode switch service
 *
 * This creates and registers the custom GATT service that allows clients
 * to switch between broadcast sink and source modes.
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int mode_switch_service_start(void);

/**
 * @brief Stop the mode switch service
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int mode_switch_service_stop(void);

/**
 * @brief Get the current mode
 *
 * @return Current mode (MODE_SWITCH_IDLE, MODE_SWITCH_SINK, MODE_SWITCH_SOURCE)
 */
mode_switch_type_t mode_switch_service_get_mode(void);

/**
 * @brief Set the current mode
 *
 * @param mode New mode (MODE_SWITCH_IDLE, MODE_SWITCH_SINK, MODE_SWITCH_SOURCE)
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int mode_switch_service_set_mode(mode_switch_type_t mode);



#endif /* MODE_SWITCH_SERVICE_H_ */
