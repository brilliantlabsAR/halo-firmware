/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef POWER_MGR_H_
#define POWER_MGR_H_
#include <inttypes.h>
#include <stdbool.h>
typedef enum {
	PM_STATE_MODE_IDLE,
	PM_STATE_MODE_STANDBY,
	PM_STATE_MODE_STOP
} pm_state_mode_type_e;

typedef enum {
	PM_WAKEUP_BLE,
	PM_WAKEUP_RTC,
	PM_WAKEUP_LPGPIO0,
	PM_WAKEUP_LPGPIO1,
} pm_wakeup_source_e;

int power_mgr_set_offprofile(pm_state_mode_type_e pm_mode);
void power_mgr_ready_for_sleep(void);
bool power_mgr_cold_boot(void);
uint32_t power_mgr_get_wakeup_reason(void);

/**
 * Wakeup reason cross-checked against the SE off profile's armed wake events:
 * pending IRQs that were not armed are boot-time artifacts and are ignored
 * (e.g. an idle-timer RTC compare firing while MCUboot ran). Falls back to
 * the raw NVIC decode when the SE is unavailable or the result is ambiguous.
 */
uint32_t power_mgr_resolve_wakeup_reason(void);
void power_mgr_set_subsys_off_period(uint32_t period_ms);
int power_mgr_set_rtc_wakeup_enable(bool enable);

/**
 * Select which sources may wake the SoC from the SE off profile (SOFT_OFF).
 * Reads the current off config and only writes it when the selection changes.
 *
 * button = LPGPIO1, mic = LPGPIO0 (AAD), rtc = LPRTC alarm.
 */
int power_mgr_set_wakeup_sources(bool button, bool mic, bool rtc);

/**
 * NVIC ISPR[0..1] captured at PRE_KERNEL_1, before drivers service or clear
 * the wake IRQs — records which interrupt(s) actually woke the SoC.
 */
void power_mgr_get_boot_pending(uint32_t *ispr0, uint32_t *ispr1);

#endif /*POWER_MGR_H_*/
