/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef _MAIN_H
#define _MAIN_H

#include "gap.h"

/**
 * @brief Set the red LED state.
 *
 * @param state true to turn on the LED, false to turn it off.
 */
void set_red_led(bool state);

/**
 * @brief Set the blue LED state.
 *
 * @param state true to turn on the LED, false to turn it off.
 */
void set_blue_led(bool state);

/**
 * @brief Set the green LED state.
 *
 * @param state true to turn on the LED, false to turn it off.
 */
void set_green_led(bool state);

/**
 * @brief Indicate that a peer is found.
 *
 * @param p_addr The address of the peer device.
 * @return int 0 on success, negative error code on failure.
 */
int peer_found(gap_bdaddr_t const *const p_addr);

/**
 * @brief Indicate that a peer is ready.
 *
 * @param conidx The connection index of the peer.
 * @return int 0 on success, negative error code on failure.
 */
int peer_ready(uint32_t conidx);

#endif /* _MAIN_H */
