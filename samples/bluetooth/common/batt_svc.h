/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef BATT_SVC_H
#define BATT_SVC_H

void config_battery_service(void);
void battery_process(void);
void update_bass_env(uint16_t ccc_bf_u, bool ready_to_send_u);
uint16_t get_batt_id(void);

#endif /* BATT_SVC_H */
