/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef SHARED_CONTROL_H
#define SHARED_CONTROL_H

/* 
 * The shared structure holding connection-related information.
 */
struct shared_control {
	bool connected;
	uint8_t status;
	uint8_t error;
};

/* Share connection status info to services controllers */
void service_conn(struct shared_control *ctrl);

#endif /* SHARED_CONTROL_H */
