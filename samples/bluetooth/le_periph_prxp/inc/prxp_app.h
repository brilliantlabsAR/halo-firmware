/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef PRXP_APP_H
#define PRXP_APP_H

extern bool config_ready;
void server_configure(void);
void get_tx_power(void);
void ias_process(void);
void tx_power_read(void);
void disc_notify(uint16_t reason);
gapm_callbacks_t append_cbs(gapm_callbacks_t *gapm_append_cbs);
gapm_le_adv_create_param_t append_adv_param(gapm_le_adv_create_param_t *adv_append_params);

#endif /* PRXP_APP_H */
