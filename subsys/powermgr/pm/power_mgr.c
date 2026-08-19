/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/device.h>
#include <zephyr/pm/pm.h>
#include <zephyr/pm/policy.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/dt-bindings/pinctrl/balletto-pinctrl.h>
#include <se_service.h>
#include <stdlib.h>
#include <inttypes.h>
#include <cmsis_core.h>
#include <soc.h>
#include <pm_rtss.h>
#include <power_mgr.h>

#define LOG_MODULE_NAME alif_power_mgr_lib
#define LOG_LEVEL       LOG_LEVEL_INFO

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

#if DT_NODE_HAS_COMPAT_STATUS(DT_NODELABEL(rtc0), snps_dw_apb_rtc, okay)
#define WAKEUP_SOURCE      DT_NODELABEL(rtc0)
#define WAKEUP_SOURCE_IRQ  DT_IRQ_BY_IDX(WAKEUP_SOURCE, 0, irq)
#define WAKEUP_EVENT       WE_LPRTC | WE_LPGPIO0 | WE_LPGPIO1
#define WAKEUP_EWIC_CFG    EWIC_RTC_A | EWIC_VBAT_GPIO
#define WAKEUP_LPGPIO0_IRQ DT_IRQ_BY_IDX(DT_NODELABEL(lpgpio), 0, irq)
#define WAKEUP_LPGPIO1_IRQ DT_IRQ_BY_IDX(DT_NODELABEL(lpgpio), 1, irq)
#elif DT_NODE_HAS_COMPAT_STATUS(DT_NODELABEL(timer0), snps_dw_timer, okay)
#define WAKEUP_SOURCE      DT_NODELABEL(timer0)
#define WAKEUP_SOURCE_IRQ  DT_IRQ_BY_IDX(WAKEUP_SOURCE, 0, irq)
#define WAKEUP_EVENT       WE_LPRTC
#define WAKEUP_EWIC_CFG    EWIC_RTC_A
#define WAKEUP_LPGPIO0_IRQ DT_IRQ_BY_IDX(DT_NODELABEL(lpgpio), 0, irq)
#define WAKEUP_LPGPIO1_IRQ DT_IRQ_BY_IDX(DT_NODELABEL(lpgpio), 1, irq)
#else
#error "RTC0 or Timer 0 not available"
#endif

static uint32_t wakeup_reason;
static bool cold_boot;
/* NVIC pending state captured at PRE_KERNEL_1, before any driver services or
 * clears the wake IRQs — forensic record of what actually woke the SoC. */
static uint32_t boot_nvic_ispr0;
static uint32_t boot_nvic_ispr1;
static bool boot_pend_rtc;
static bool boot_pend_lpgpio0;
static bool boot_pend_lpgpio1;

/* The console UART stays on SYST_PCLK, the clock the SoC init selects
 * (soc_b1_dk_rtss_he.c) and that the SE maintains across power profiles.
 * It used to be switched to HFOSC here at PRE_KERNEL_1/50 — after the UART
 * driver had already programmed its divisor at priority 40 — but HFOSC is
 * an RC oscillator no run/off profile requests, so the SE left it
 * unmaintained during idle and the console garbled until the first wake
 * (#249). HFOSC itself is still enabled by the board init when
 * CONFIG_MIPI_DSI needs it.
 */

static inline uint32_t get_wakeup_irq_status(void)
{
	/* Priority order, not addition: with more than one wake IRQ pending the
	 * old additive encoding (RTC*1 + LPGPIO0*2 + LPGPIO1*3) produced values
	 * that mapped to the wrong source or to none at all. */
	if (NVIC_GetPendingIRQ(WAKEUP_LPGPIO1_IRQ)) {
		return PM_WAKEUP_LPGPIO1;
	}
	if (NVIC_GetPendingIRQ(WAKEUP_LPGPIO0_IRQ)) {
		return PM_WAKEUP_LPGPIO0;
	}
	if (NVIC_GetPendingIRQ(WAKEUP_SOURCE_IRQ)) {
		return PM_WAKEUP_RTC;
	}
	return PM_WAKEUP_BLE;
}

/*
 * This function will be invoked in the PRE_KERNEL_1 phase of the init
 * routine to prevent sleep during startup.
 */
static int app_pre_kernel_init(void)
{
	boot_nvic_ispr0 = NVIC->ISPR[0];
	boot_nvic_ispr1 = NVIC->ISPR[1];
	boot_pend_rtc = NVIC_GetPendingIRQ(WAKEUP_SOURCE_IRQ);
	boot_pend_lpgpio0 = NVIC_GetPendingIRQ(WAKEUP_LPGPIO0_IRQ);
	boot_pend_lpgpio1 = NVIC_GetPendingIRQ(WAKEUP_LPGPIO1_IRQ);
	wakeup_reason = get_wakeup_irq_status();
	pm_policy_state_lock_get(PM_STATE_SOFT_OFF, PM_ALL_SUBSTATES);

	return 0;
}
SYS_INIT(app_pre_kernel_init, PRE_KERNEL_1, 39);

static int pm_application_init(void)
{
	if (!balletto_vbat_resume_enabled()) {
		/* Mark a cold boot */
		cold_boot = true;
	}

	return 0;
}
SYS_INIT(pm_application_init, PRE_KERNEL_1, 3); /*CONFIG_SE_SERVICE_INIT_PRIORITY + 3 */

bool power_mgr_cold_boot(void)
{
	return cold_boot;
}

uint32_t power_mgr_get_wakeup_reason(void)
{
	return wakeup_reason;
}

uint32_t power_mgr_resolve_wakeup_reason(void)
{
	off_profile_t offp = {0};

	if (se_service_get_off_cfg(&offp) != 0) {
		return wakeup_reason;
	}

	/* Only events that were armed in the off profile can have woken the
	 * SoC. Pending IRQs outside the armed set are boot-time artifacts:
	 * MCUboot's button probe clears the LPGPIO1 pend, while the idle-timer
	 * RTC compare (left armed from before power-off) can fire during the
	 * bootloader and set a fresh, unrelated RTC pend. */
	if ((offp.wakeup_events & WE_LPGPIO1) && boot_pend_lpgpio1) {
		return PM_WAKEUP_LPGPIO1;
	}
	if ((offp.wakeup_events & WE_LPGPIO0) && boot_pend_lpgpio0) {
		return PM_WAKEUP_LPGPIO0;
	}
	if ((offp.wakeup_events & WE_LPRTC) && boot_pend_rtc) {
		return PM_WAKEUP_RTC;
	}

	/* Nothing both armed and pending (pends consumed during boot): if the
	 * armed set is unambiguous, that source is the only possible waker. */
	if (offp.wakeup_events == WE_LPGPIO1) {
		return PM_WAKEUP_LPGPIO1;
	}
	if (offp.wakeup_events == WE_LPRTC) {
		return PM_WAKEUP_RTC;
	}

	return wakeup_reason;
}

int power_mgr_set_offprofile(pm_state_mode_type_e pm_mode)
{
	int ret;
	off_profile_t offp = {0};

	if (!balletto_vbat_resume_enabled()) {

		const struct device *const wakeup_dev = DEVICE_DT_GET(WAKEUP_SOURCE);

		balletto_vbat_resume_enable();

		if (!device_is_ready(wakeup_dev)) {
			LOG_ERR("%s: device not ready", wakeup_dev->name);
			return -1;
		}

		counter_start(wakeup_dev);
	}

	/* Set default for stop mode with RTC wakeup support */
	offp.power_domains = PD_VBAT_AON_MASK;
	offp.memory_blocks = MRAM_MASK;
	offp.memory_blocks |= SERAM_1_MASK | SERAM_2_MASK | SERAM_3_MASK | SERAM_4_MASK;
	offp.memory_blocks |=
		SRAM5_1_MASK | SRAM5_2_MASK | SRAM5_3_MASK | SRAM5_4_MASK | SRAM5_5_MASK;
	offp.dcdc_voltage = CONFIG_SOC_B1_DCDC_VOLTAGE;

	switch (pm_mode) {
	case PM_STATE_MODE_IDLE:
		offp.power_domains |= PD_SYST_MASK | PD_SSE700_AON_MASK | PD_SESS_MASK;
		offp.memory_blocks |= SRAM5_3_MASK;
		offp.ip_clock_gating = LDO_PHY_MASK;
		offp.phy_pwr_gating = LDO_PHY_MASK;
		offp.dcdc_mode = DCDC_MODE_PFM_FORCED;
		break;
	case PM_STATE_MODE_STANDBY:
		offp.power_domains |= PD_SSE700_AON_MASK;
		offp.memory_blocks |= SRAM5_3_MASK;
		offp.ip_clock_gating = 0;
		offp.phy_pwr_gating = 0;
		offp.dcdc_mode = DCDC_MODE_OFF;
		break;
	case PM_STATE_MODE_STOP:
		offp.ip_clock_gating = 0;
		offp.phy_pwr_gating = 0;
		offp.dcdc_mode = DCDC_MODE_OFF;
		break;
	}

	offp.aon_clk_src = CLK_SRC_LFXO;
	offp.stby_clk_src = CLK_SRC_HFRC;
	offp.stby_clk_freq = SCALED_FREQ_RC_STDBY_76_8_MHZ;
	offp.ewic_cfg = WAKEUP_EWIC_CFG;
	offp.wakeup_events = WAKEUP_EVENT;
	/* Route wake-from-off through the full boot chain (SE -> MCUboot ->
	 * app) by vectoring at the bootloader, not the running app image
	 * (SCB->VTOR). Direct-to-app warm boots skip the cold-path platform
	 * init and crash in BLE bring-up, causing a double boot on wake. */
	offp.vtor_address = CONFIG_FLASH_BASE_ADDRESS;
	offp.vtor_address_ns = CONFIG_FLASH_BASE_ADDRESS;

	ret = se_service_set_off_cfg(&offp);
	if (ret) {
		LOG_ERR("SE: set_off_cfg failed = %d", ret);
	} else {
		if (IS_ENABLED(CONFIG_SOC_B1_DK_RTSS_HE)) {
			/* Set DCDC */
			sys_write32(0x0a004411, 0x1a60a034);
			sys_write32(0x1e11e701, 0x1a60a030);
		}
	}

	return ret;
}

void power_mgr_ready_for_sleep(void)
{
	pm_policy_state_lock_put(PM_STATE_SOFT_OFF, PM_ALL_SUBSTATES);
}

void power_mgr_set_subsys_off_period(uint32_t period_ms)
{
	pm_policy_state_lock_put(PM_STATE_SOFT_OFF, PM_ALL_SUBSTATES);
	k_sleep(K_MSEC(period_ms));
	pm_policy_state_lock_get(PM_STATE_SOFT_OFF, PM_ALL_SUBSTATES);
}

void power_mgr_get_boot_pending(uint32_t *ispr0, uint32_t *ispr1)
{
	if (ispr0) {
		*ispr0 = boot_nvic_ispr0;
	}
	if (ispr1) {
		*ispr1 = boot_nvic_ispr1;
	}
}

int power_mgr_set_wakeup_sources(bool button, bool mic, bool rtc)
{
	int ret;
	off_profile_t offp = {0};
	uint32_t we = 0;
	uint32_t ewic = 0;

	if (button) {
		we |= WE_LPGPIO1;
	}
	if (mic) {
		we |= WE_LPGPIO0;
	}
	if (button || mic) {
		ewic |= EWIC_VBAT_GPIO;
	}
	if (rtc) {
		we |= WE_LPRTC;
		ewic |= EWIC_RTC_A;
	}

	ret = se_service_get_off_cfg(&offp);
	if (ret) {
		LOG_ERR("Failed to get off cfg: %d", ret);
		return ret;
	}

	if (offp.wakeup_events == we && offp.ewic_cfg == ewic &&
	    offp.vtor_address == CONFIG_FLASH_BASE_ADDRESS) {
		return 0;
	}

	offp.wakeup_events = we;
	offp.ewic_cfg = ewic;
	/* Keep wake routed through the boot chain (see power_mgr_set_offprofile) */
	offp.vtor_address = CONFIG_FLASH_BASE_ADDRESS;
	offp.vtor_address_ns = CONFIG_FLASH_BASE_ADDRESS;

	ret = se_service_set_off_cfg(&offp);
	if (ret) {
		LOG_ERR("Failed to set off cfg: %d", ret);
		return ret;
	}

	LOG_INF("Wakeup sources: button=%d mic=%d rtc=%d", button, mic, rtc);
	return 0;
}

int power_mgr_set_rtc_wakeup_enable(bool enable)
{
	int ret;
	off_profile_t offp = {0};

	/* Get current off profile configuration */
	ret = se_service_get_off_cfg(&offp);
	if (ret) {
		LOG_ERR("Failed to get off cfg: %d", ret);
		return ret;
	}

	/* Check if state actually needs to change */
	bool current_enabled = (offp.wakeup_events & WE_LPRTC) != 0;
	if (current_enabled == enable) {
		/* Already in desired state, no need to update */
		return 0;
	}


	/* Modify wakeup events and EWIC configuration based on enable flag */
	if (enable) {
		/* Enable RTC wakeup */
		offp.wakeup_events |= WE_LPRTC;
		offp.ewic_cfg |= EWIC_RTC_A;
		LOG_DBG("RTC wakeup enabled");
	} else {
		/* Disable RTC wakeup - keep other wakeup sources (LPGPIO0/1) */
		offp.wakeup_events &= ~WE_LPRTC;
		offp.ewic_cfg &= ~EWIC_RTC_A;
		LOG_DBG("RTC wakeup disabled");
	}

	/* Apply the new configuration */
	ret = se_service_set_off_cfg(&offp);
	if (ret) {
		LOG_ERR("Failed to set off cfg: %d", ret);
		return ret;
	}

	return 0;
}
