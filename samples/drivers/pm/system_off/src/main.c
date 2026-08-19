/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https: //alifsemi.com/license
 *
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/pm/pm.h>
#include <zephyr/pm/policy.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/drivers/counter.h>
#include <cmsis_core.h>
#include <soc.h>
#include <se_service.h>

/**
 * Enable this if we need systop to be ON in the early boot.
 *
 * We normally use SE Services to enable the SYSTOP. In the warmboot, depending on the
 * power domain set in the OFF profile, SYSTOP might not be ON. The SE services will be invoked
 * (much later in the boot process) after the devices are configured. So the devices in the SYSTOP
 * region, may not be configured correctly.
 */
#define EARLY_BOOT_SYSTOP_ON 1

/**
 * As per the application requirements, it can remove the memory blocks which are not in use.
 */
#if defined(CONFIG_SOC_SERIES_ENSEMBLE_E1C) || defined(CONFIG_SOC_SERIES_BALLETTO_B1)
	#define APP_RET_MEM_BLOCKS SRAM4_1_MASK | SRAM4_2_MASK | SRAM4_3_MASK | SRAM4_4_MASK | \
					SRAM5_1_MASK | SRAM5_2_MASK | SRAM5_3_MASK | SRAM5_4_MASK |\
					SRAM5_5_MASK
	#define SERAM_MEMORY_BLOCKS_IN_USE SERAM_1_MASK | SERAM_2_MASK | SERAM_3_MASK | SERAM_4_MASK
#else
	#define APP_RET_MEM_BLOCKS SRAM4_1_MASK | SRAM4_2_MASK | SRAM5_1_MASK | SRAM5_2_MASK
	#define SERAM_MEMORY_BLOCKS_IN_USE SERAM_MASK
#endif

#if DT_NODE_HAS_COMPAT_STATUS(DT_NODELABEL(rtc0), snps_dw_apb_rtc, okay)
	#define WAKEUP_SOURCE DT_NODELABEL(rtc0)
	#define SE_OFFP_EWIC_CFG EWIC_RTC_A
	#define SE_OFFP_WAKEUP_EVENTS WE_LPRTC
#elif DT_NODE_HAS_COMPAT_STATUS(DT_NODELABEL(timer0), snps_dw_timers, okay)
	#define WAKEUP_SOURCE DT_NODELABEL(timer0)
	#define SE_OFFP_EWIC_CFG EWIC_VBAT_TIMER
	#define SE_OFFP_WAKEUP_EVENTS WE_LPTIMER0
#else
#error "Wakeup Device not enabled in the dts"
#endif

#define WAKEUP_SOURCE_IRQ DT_IRQ_BY_IDX(WAKEUP_SOURCE, 0, irq)

#define NORMAL_SLEEP_IN_USEC (3 * 1000 * 1000)
#define DEEP_SLEEP_IN_USEC (20 * 1000 * 1000)

#define OFF_STATE_NODE_ID DT_PHANDLE_BY_IDX(DT_NODELABEL(cpu0), cpu_power_states, 0)
BUILD_ASSERT((NORMAL_SLEEP_IN_USEC < DT_PROP_OR(OFF_STATE_NODE_ID, min_residency_us, 0)),
	"Normal Sleep should be less than min-residency-us");


#define SOC_STANDBY_MODE_PD PD_SSE700_AON_MASK
#define SOC_STOP_MODE_PD PD_VBAT_AON_MASK

/**
 * By default STOP mode is requested.
 * For Standby, set the SOC_REQUESTED_POWER_MODE to SOC_STANDBY_MODE_PD
 */
#define SOC_REQUESTED_POWER_MODE SOC_STOP_MODE_PD

static uint32_t wakeup_reason;

static inline uint32_t get_wakeup_irq_status(void)
{
	return NVIC_GetPendingIRQ(WAKEUP_SOURCE_IRQ);
}

#ifdef EARLY_BOOT_SYSTOP_ON
/**
 * This function will be invoked in the PRE_KERNEL_1 phase of the init routine.
 * This is required to do only when we need the SYSTOP to be ON.
 *
 * This will make sure SYSTOP be ON before initializing the peripherals.
 */
static uint32_t host_bsys_pwr_req;
#define HOST_SYSTOP_PWR_REQ_LOGIC_ON_MEM_ON 0x20

static inline void app_force_host_systop_on(void)
{
	host_bsys_pwr_req = sys_read32(HOST_BSYS_PWR_REQ);
	sys_write32(host_bsys_pwr_req | HOST_SYSTOP_PWR_REQ_LOGIC_ON_MEM_ON, HOST_BSYS_PWR_REQ);
}

static inline void app_restore_host_systop(void)
{
	sys_write32(host_bsys_pwr_req, HOST_BSYS_PWR_REQ);
}

static int app_pre_kernel1_init(void)
{
	app_force_host_systop_on();

	return 0;
}
SYS_INIT(app_pre_kernel1_init, PRE_KERNEL_1, 39); /* (CONFIG_KERNEL_INIT_PRIORITY_DEFAULT - 1) */
#endif

/**
 * Use the HFOSC clock for the UART console
 */
#if DT_SAME_NODE(DT_NODELABEL(uart4), DT_CHOSEN(zephyr_console))
#define CONSOLE_UART_NUM 4
#elif DT_SAME_NODE(DT_NODELABEL(uart2), DT_CHOSEN(zephyr_console))
#define CONSOLE_UART_NUM 2
#else
#error "Specify the uart console number"
#endif

#define UART_CTRL_CLK_SEL_POS 8

static int app_pre_console_init(void)
{
	/* Enable HFOSC in CGU */
	sys_set_bits(CGU_CLK_ENA, BIT(23));

	/* Enable HFOSC for the UART console */
	sys_clear_bits(EXPSLV_UART_CTRL, BIT((CONSOLE_UART_NUM + UART_CTRL_CLK_SEL_POS)));

	return 0;
}
SYS_INIT(app_pre_console_init, PRE_KERNEL_1, 50);

/*
 * This function will be invoked in the PRE_KERNEL_2 phase of the init routine.
 * We can read the wakeup reason from reading the RESET STATUS register
 * and from the pending IRQ.
 */
static int app_pre_kernel_init(void)
{
	wakeup_reason = get_wakeup_irq_status();

	pm_policy_state_lock_get(PM_STATE_SOFT_OFF, PM_ALL_SUBSTATES);

	return 0;
}
SYS_INIT(app_pre_kernel_init, PRE_KERNEL_2, 0);

/**
 * Set the RUN profile parameters for this application.
 */
static int app_set_run_params(void)
{
	run_profile_t runp;
	int ret;

	ret = se_service_sync();
	if (ret) {
		printk("SE: not responding to service calls %d\n", ret);
		return 0;
	}

	ret = se_service_get_run_cfg(&runp);
	if (ret) {
		printk("SE: get_run_cfg failed = %d.\n", ret);
		return 0;
	}

	runp.power_domains = PD_SYST_MASK | PD_SSE700_AON_MASK;
	runp.dcdc_voltage  = 825;
	runp.dcdc_mode     = DCDC_MODE_PWM;
	runp.aon_clk_src   = CLK_SRC_LFXO;
	runp.run_clk_src   = CLK_SRC_PLL;
#if defined(CONFIG_RTSS_HP)
	runp.cpu_clk_freq  = CLOCK_FREQUENCY_400MHZ;
#else
	runp.cpu_clk_freq  = CLOCK_FREQUENCY_160MHZ;
#endif
	if (SCB->VTOR) {
		runp.memory_blocks |= MRAM_MASK;
	}

	ret = se_service_set_run_cfg(&runp);
	if (ret) {
		printk("SE: set_run_cfg failed = %d.\n", ret);
		return 0;
	}
#ifdef EARLY_BOOT_SYSTOP_ON
	app_restore_host_systop();
#endif

	return 0;
}

static int app_set_off_params(void)
{
	int ret;
	off_profile_t offp;

	ret = se_service_get_off_cfg(&offp);
	if (ret) {
		printk("SE: get_off_cfg failed = %d.\n", ret);
		printk("ERROR: Can't establish SE connection, app exiting..\n");
		return ret;
	}

	offp.power_domains = SOC_REQUESTED_POWER_MODE;
	offp.aon_clk_src   = CLK_SRC_LFXO;
	offp.stby_clk_src  = CLK_SRC_HFXO;
	offp.ewic_cfg      = SE_OFFP_EWIC_CFG;
	offp.wakeup_events = SE_OFFP_WAKEUP_EVENTS;
	offp.vtor_address  = SCB->VTOR;
	offp.memory_blocks = MRAM_MASK;

#if defined(CONFIG_RTSS_HE)
	/*
	 * Enable the HE TCM retention only if the VTOR is present.
	 * This is just for this test application.
	 */
	if (!SCB->VTOR) {
		offp.memory_blocks = APP_RET_MEM_BLOCKS | SERAM_MEMORY_BLOCKS_IN_USE;
	} else {
		offp.memory_blocks |= SERAM_MEMORY_BLOCKS_IN_USE;
	}
#else
	/*
	 * Retention is not possible with HP-TCM
	 */
	if (SCB->VTOR) {
		printf("\r\nHP TCM Retention is not possible\n");
		printk("ERROR: VTOR is set to TCM, app exiting..\n");
		return ret;
	}

	offp.memory_blocks = MRAM_MASK;
#endif

	printk("SE: VTOR = %x\n", offp.vtor_address);
	printk("SE: MEMBLOCKS = %x\n", offp.memory_blocks);

	ret = se_service_set_off_cfg(&offp);
	if (ret) {
		printk("SE: set_off_cfg failed = %d.\n", ret);
		printk("ERROR: Can't establish SE connection, app exiting..\n");
		return ret;
	}

	return 0;
}

#if !defined(CONFIG_CORTEX_M_SYSTICK_IDLE_TIMER)
static volatile uint32_t alarm_cb_status;
static void alarm_callback_fn(const struct device *wakeup_dev,
				uint8_t chan_id, uint32_t ticks,
				void *user_data)
{
	printk("%s: !!! Alarm !!!\n", wakeup_dev->name);
	alarm_cb_status = 1;
}
#endif

static int app_enter_normal_sleep(uint32_t sleep_usec)
{
#if defined(CONFIG_CORTEX_M_SYSTICK_IDLE_TIMER)
	k_sleep(K_USEC(sleep_usec));
#else
	const struct device *const wakeup_dev = DEVICE_DT_GET(WAKEUP_SOURCE);
	struct counter_alarm_cfg alarm_cfg;
	int ret;

	alarm_cfg.flags = 0;
	alarm_cfg.ticks = counter_us_to_ticks(wakeup_dev, sleep_usec);
	alarm_cfg.callback = alarm_callback_fn;
	alarm_cfg.user_data = &alarm_cfg;

	ret = counter_set_channel_alarm(wakeup_dev, 0, &alarm_cfg);
	if (ret) {
		printk("Couldnt set the alarm\n");
		return ret;
	}
	printk("Set alarm for %u microseconds\n", sleep_usec);

	k_sleep(K_USEC(sleep_usec));

	if (!alarm_cb_status) {
		return -1;
	}
	alarm_cb_status = 0;


#endif
	return 0;
}

static int app_enter_deep_sleep(uint32_t sleep_usec)
{
#if defined(CONFIG_CORTEX_M_SYSTICK_IDLE_TIMER)
	/**
	 * Set a delay more than the min-residency-us configured so that
	 * the sub-system will go to OFF state.
	 */
	k_sleep(K_USEC(sleep_usec));
#else
	const struct device *const wakeup_dev = DEVICE_DT_GET(WAKEUP_SOURCE);
	struct counter_alarm_cfg alarm_cfg;
	int ret;
	/*
	 * Set the alarm and delay so that idle thread can run
	 */
	alarm_cfg.ticks = counter_us_to_ticks(wakeup_dev, sleep_usec);
	ret = counter_set_channel_alarm(wakeup_dev, 0, &alarm_cfg);
	if (ret) {
		printk("Failed to set the alarm (err %d)", ret);
		return ret;
	}

	printk("Set alarm for %u microseconds\n\n", sleep_usec);

	if (ret) {
		printk("Couldnt set the alarm\n");
		return ret;
	}

	sys_poweroff();

#endif

	return 0;
}

int main(void)
{
	const struct device *const cons = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	const struct device *const wakeup_dev = DEVICE_DT_GET(WAKEUP_SOURCE);
	int ret;

	if (!device_is_ready(cons)) {
		printk("%s: device not ready.\n", cons->name);
		printk("ERROR: app exiting..\n");
		return 0;
	}

	if (!device_is_ready(wakeup_dev)) {
		printk("%s: device not ready.\n", wakeup_dev->name);
		printk("ERROR: app exiting..\n");
		return 0;
	}

	printk("\n%s System Off Demo\n", CONFIG_BOARD);

	if (wakeup_reason) {
		printk("\r\nWakeup Interrupt Reason : %s\n\n", wakeup_dev->name);
	}

	ret = app_set_run_params();
	if (ret) {
		printk("ERROR: app exiting..\n");
		return 0;
	}

	ret = app_set_off_params();
	if (ret) {
		printk("ERROR: app exiting..\n");
		return 0;
	}

	pm_policy_state_lock_put(PM_STATE_SOFT_OFF, PM_ALL_SUBSTATES);

	ret = counter_start(wakeup_dev);
	if (ret) {
		printk("Failed to start counter (err %d)", ret);
		printk("ERROR: app exiting..\n");
		return 0;
	}

	printk("\nEnter Normal Sleep for (%d microseconds)\n", NORMAL_SLEEP_IN_USEC);
	ret = app_enter_normal_sleep(NORMAL_SLEEP_IN_USEC);
	if (ret) {
		printk("ERROR: app exiting..\n");
		return 0;
	}

	printk("Exited from Normal Sleep\n\n");

	printk("\nEnter Subsystem OFF\n");
	printk("SoC may go to STOP/STANDBY/IDLE depending on the global power mode\n");

	printk("\nEnter Deep Sleep for (%d microseconds)\n", DEEP_SLEEP_IN_USEC);
	ret = app_enter_deep_sleep(DEEP_SLEEP_IN_USEC);
	if (ret) {
		printk("ERROR: app exiting..\n");
		return 0;
	}

	printk("ERROR: Failed to enter Subsystem OFF\n");
	while (true) {
		/* spin here */
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
