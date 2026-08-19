/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/shell/shell.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/dt-bindings/pinctrl/balletto-pinctrl.h>
#if defined(CONFIG_PM)
#include <power_mgr.h>
#endif
#include <es0_power_manager.h>
#include "se_service.h"
#include <stdlib.h>
#include <inttypes.h>

#define LOG_MODULE_NAME alif_power_mgr_shell
#define LOG_LEVEL       LOG_LEVEL_INFO

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

#define NVD_BOOT_PARAMS_MAX_SIZE (512)

#define LL_CLK_SEL_CTRL_REG_ADDR   0x1A60201C
#define LL_UART_CLK_SEL_CTRL_16MHZ 0x00
#define LL_UART_CLK_SEL_CTRL_24MHZ 0x01
#define LL_UART_CLK_SEL_CTRL_48MHZ 0x03

/* Tag status: (STATUS_VALID | STATUS_NOT_LOCKED | STATUS_NOT_ERASED) */
#define DEFAULT_TAG_STATUS (0x00 | 0x02 | 0x04)

/* Boot time value definitions */
#define BOOT_PARAM_ID_LE_CODED_PHY_500          0x85
#define BOOT_PARAM_ID_DFT_SLAVE_MD              0x20
#define BOOT_PARAM_ID_CH_CLASS_REP_INTV         0x36
#define BOOT_PARAM_ID_BD_ADDRESS                0x01
#define BOOT_PARAM_ID_ACTIVITY_MOVE_CONFIG      0x15
#define BOOT_PARAM_ID_SCAN_EXT_ADV              0x16
#define BOOT_PARAM_ID_RSSI_HIGH_THR             0x3A
#define BOOT_PARAM_ID_RSSI_LOW_THR              0x3B
#define BOOT_PARAM_ID_SLEEP_ENABLE              0x11
#define BOOT_PARAM_ID_EXT_WAKEUP_ENABLE         0x12
#define BOOT_PARAM_ID_ENABLE_CHANNEL_ASSESSMENT 0x19
#define BOOT_PARAM_ID_RSSI_INTERF_THR           0x3C
#define BOOT_PARAM_ID_UART_BAUDRATE             0x10
#define BOOT_PARAM_ID_UART_INPUT_CLK_FREQ       0xC0
#define BOOT_PARAM_ID_NO_PARAM                  0xFF
#define BOOT_PARAM_ID_EXT_WAKEUP_TIME           0x0D
#define BOOT_PARAM_ID_OSC_WAKEUP_TIME           0x0E
#define BOOT_PARAM_ID_RM_WAKEUP_TIME            0x0F
#define BOOT_PARAM_ID_EXT_WARMBOOT_WAKEUP_TIME  0xD0
#define BOOT_PARAM_ID_LPCLK_DRIFT               0x07
#define BOOT_PARAM_ID_ACTCLK_DRIFT              0x09
#define BOOT_PARAM_ID_CONFIGURATION             0xD1

#define BOOT_PARAM_LEN_LE_CODED_PHY_500          1
#define BOOT_PARAM_LEN_DFT_SLAVE_MD              1
#define BOOT_PARAM_LEN_CH_CLASS_REP_INTV         2
#define BOOT_PARAM_LEN_BD_ADDRESS                6
#define BOOT_PARAM_LEN_ACTIVITY_MOVE_CONFIG      1
#define BOOT_PARAM_LEN_SCAN_EXT_ADV              1
#define BOOT_PARAM_LEN_RSSI_THR                  1
#define BOOT_PARAM_LEN_SLEEP_ENABLE              1
#define BOOT_PARAM_LEN_EXT_WAKEUP_ENABLE         1
#define BOOT_PARAM_LEN_ENABLE_CHANNEL_ASSESSMENT 1
#define BOOT_PARAM_LEN_UART_BAUDRATE             4
#define BOOT_PARAM_LEN_UART_INPUT_CLK_FREQ       4
#define BOOT_PARAM_LEN_EXT_WAKEUP_TIME           2
#define BOOT_PARAM_LEN_OSC_WAKEUP_TIME           2
#define BOOT_PARAM_LEN_RM_WAKEUP_TIME            2
#define BOOT_PARAM_LEN_EXT_WARMBOOT_WAKEUP_TIME  2
#define BOOT_PARAM_LEN_LPCLK_DRIFT               2
#define BOOT_PARAM_LEN_ACTCLK_DRIFT              1
#define BOOT_PARAM_LEN_CONFIGURATION             4

#define CONFIGURATION_RF_TYPE_HPA		 1
#define CONFIGURATION_SOC_TYPE_CSP		 2

#define ES0_PM_ERROR_NO_ERROR             0
#define ES0_PM_ERROR_TOO_MANY_USERS       -1
#define ES0_PM_ERROR_TOO_MANY_BOOT_PARAMS -2
#define ES0_PM_ERROR_INVALID_BOOT_PARAMS  -3
#define ES0_PM_ERROR_START_FAILED         -4
#define ES0_PM_ERROR_NO_BAUDRATE          -5
#define ES0_PM_ERROR_BAUDRATE_MISMATCH    -6

static uint8_t *write_tlv_int(uint8_t *target, uint8_t tag, uint32_t value, uint8_t len)
{
	*target++ = tag;
	*target++ = DEFAULT_TAG_STATUS;
	*target++ = len;
	if (len == 1) {
		memcpy(target, (uint8_t *)&value, len);
	} else if (len == 2) {
		memcpy(target, (uint16_t *)&value, len);
	} else {
		memcpy(target, &value, len);
	}

	target += len;

	return target;
}

static uint8_t *write_tlv_str(uint8_t *target, uint8_t tag, const void *value, uint8_t len)
{
	*target++ = tag;
	*target++ = DEFAULT_TAG_STATUS;
	*target++ = len;

	memcpy(target, value, len);

	target += len;

	return target;
}

static const void *bdaddr_reverse(const uint8_t src[6])
{
	static uint8_t rev[6];

	for (int i = 0; i < 6; ++i) {
		rev[i] = src[5 - i];
	}

	return rev;
}

static void alif_eui48_read(uint8_t *eui48)
{
#ifdef ALIF_IEEE_MA_L_IDENTIFIER
	eui48[0] = (uint8_t)(ALIF_IEEE_MA_L_IDENTIFIER >> 16);
	eui48[1] = (uint8_t)(ALIF_IEEE_MA_L_IDENTIFIER >> 8);
	eui48[2] = (uint8_t)(ALIF_IEEE_MA_L_IDENTIFIER);
#else
	se_service_get_rnd_num(&eui48[0], 3);
	eui48[0] |= 0xC0;
#endif
	se_system_get_eui_extension(true, &eui48[3]);
	if (eui48[3] || eui48[4] || eui48[5]) {
		return;
	}
	/* Generate Random Local value (ELI) */
	se_service_get_rnd_num(&eui48[3], 3);
}

#if defined(CONFIG_PM)
static uint32_t wakeup_counter __attribute__((noinit));
static pm_state_mode_type_e pm_off_mode __attribute__((noinit));
static int sleep_period __attribute__((noinit));

static int pm_application_init(void)
{
	if (power_mgr_cold_boot()) {
		wakeup_counter = 0;
	} else {
		if (wakeup_counter) {
			/* Set a off profile */
			if (power_mgr_set_offprofile(pm_off_mode)) {
				printk("Error to set off profile\n");
				wakeup_counter = 0;
				return 0;
			}
			wakeup_counter--;
			power_mgr_set_subsys_off_period(sleep_period);
		}
	}
	return 0;
}
SYS_INIT(pm_application_init, APPLICATION, 1);

static int64_t param_get_int(size_t argc, char **argv, char *p_param, int def_value)
{
	if (p_param && argc > 1) {
		for (int n = 0; n < (argc - 1); n++) {
			if (strcmp(argv[n], p_param) == 0) {
				return strtoll(argv[n + 1], NULL, 0);
			}
		}
	}
	return def_value;
}

static int cmd_subsys_off_configure(const struct shell *shell)
{
	int ret = power_mgr_set_offprofile(pm_off_mode);

	if (ret) {
		shell_fprintf(shell, SHELL_VT100_COLOR_DEFAULT, "ERROR: %d\n", ret);
		wakeup_counter = 0;
		return ret;
	}
	power_mgr_set_subsys_off_period(sleep_period);

	return ret;
}

static int cmd_off_test(const struct shell *shell, size_t argc, char **argv)
{
	wakeup_counter = param_get_int(argc, argv, "--cnt", 0);
	sleep_period = param_get_int(argc, argv, "--period", 1000);
	pm_off_mode = PM_STATE_MODE_STOP;
	shell_fprintf(shell, SHELL_VT100_COLOR_DEFAULT, "Start off mode %d ms period %u cnt\n",
		      sleep_period, wakeup_counter);

	return cmd_subsys_off_configure(shell);
}

static int cmd_standby_test(const struct shell *shell, size_t argc, char **argv)
{
	wakeup_counter = param_get_int(argc, argv, "--cnt", 0);
	sleep_period = param_get_int(argc, argv, "--period", 1000);
	pm_off_mode = PM_STATE_MODE_STANDBY;

	shell_fprintf(shell, SHELL_VT100_COLOR_DEFAULT, "Start Standby mode %d ms period %u cnt\n",
		      sleep_period, wakeup_counter);

	return cmd_subsys_off_configure(shell);
}

static int cmd_idle_test(const struct shell *shell, size_t argc, char **argv)
{
	wakeup_counter = param_get_int(argc, argv, "--cnt", 0);
	sleep_period = param_get_int(argc, argv, "--period", 1000);
	pm_off_mode = PM_STATE_MODE_IDLE;

	shell_fprintf(shell, SHELL_VT100_COLOR_DEFAULT, "Start Idle mode %d ms period %u cnt\n",
		      sleep_period, wakeup_counter);

	return cmd_subsys_off_configure(shell);
}
#endif

static bool param_get_flag(size_t argc, char **argv, char *p_flag)
{
	if (p_flag && argc) {
		for (int n = 0; n < argc; n++) {
			if (strcmp(argv[n], p_flag) == 0) {
				return true;
			}
		}
	}
	return false;
}

static int cmd_start(const struct shell *shell, size_t argc, char **argv)
{
	static uint8_t ll_boot_params_buffer[NVD_BOOT_PARAMS_MAX_SIZE];
	uint8_t bd_address[BOOT_PARAM_LEN_BD_ADDRESS];
	uint8_t *ptr = ll_boot_params_buffer;
	uint16_t total_length = 4; /* N,V,D,S */

	uint32_t hci_baudrate;
	uint32_t ahi_baudrate;
	uint32_t used_baudrate;
	uint32_t hpa_setup = IS_ENABLED(CONFIG_ALIF_HPA_MODE) ? 1 : 0;
	uint32_t reg_uart_clk_cfg = LL_UART_CLK_SEL_CTRL_16MHZ;
	uint32_t ll_uart_clk_freq = 16000000;
	uint32_t es0_clock_select = CONFIG_SE_SERVICE_RF_CORE_FREQUENCY;
	uint32_t min_uart_clk_freq;

	alif_eui48_read(bd_address);

	if (param_get_flag(argc, argv, "--hpa")) {
		shell_fprintf(shell, SHELL_VT100_COLOR_DEFAULT, "Enable HPA\n");
		hpa_setup = 1;
	}

	if (param_get_flag(argc, argv, "--lpa")) {
		shell_fprintf(shell, SHELL_VT100_COLOR_DEFAULT, "Enable LPA\n");
		hpa_setup = 0;
	}

	hci_baudrate = DT_PROP_OR(DT_CHOSEN(zephyr_hci_uart), current_speed, 0);
	ahi_baudrate = DT_PROP_OR(DT_CHOSEN(zephyr_ahi_uart), current_speed, 0);

	if (!hci_baudrate && !ahi_baudrate) {
		return ES0_PM_ERROR_NO_BAUDRATE;
	}

	if (hci_baudrate && ahi_baudrate && hci_baudrate != ahi_baudrate) {
		return ES0_PM_ERROR_BAUDRATE_MISMATCH;
	}

	used_baudrate = hci_baudrate ? hci_baudrate : ahi_baudrate;

	min_uart_clk_freq = used_baudrate * 16;

	memset(ll_boot_params_buffer, 0xFF, NVD_BOOT_PARAMS_MAX_SIZE);

	*ptr++ = 'N';
	*ptr++ = 'V';
	*ptr++ = 'D';
	*ptr++ = 'S';

	uint32_t config = hpa_setup ? CONFIGURATION_RF_TYPE_HPA : 0;

	if (IS_ENABLED(CONFIG_SOC_AB1C1F1M41820HH0) || IS_ENABLED(CONFIG_SOC_AB1C1F4M51820HH0)) {
		config |= CONFIGURATION_SOC_TYPE_CSP;
	}

	ptr = write_tlv_int(ptr, BOOT_PARAM_ID_LE_CODED_PHY_500, CONFIG_ALIF_PM_LE_CODED_PHY_500,
			    BOOT_PARAM_LEN_LE_CODED_PHY_500);
	ptr = write_tlv_int(ptr, BOOT_PARAM_ID_DFT_SLAVE_MD, CONFIG_ALIF_PM_DFT_SLAVE_MD,
			    BOOT_PARAM_LEN_DFT_SLAVE_MD);
	ptr = write_tlv_int(ptr, BOOT_PARAM_ID_CH_CLASS_REP_INTV, CONFIG_ALIF_PM_CH_CLASS_REP_INTV,
			    BOOT_PARAM_LEN_CH_CLASS_REP_INTV);
	ptr = write_tlv_str(ptr, BOOT_PARAM_ID_BD_ADDRESS, bdaddr_reverse(bd_address),
			    BOOT_PARAM_LEN_BD_ADDRESS);
	ptr = write_tlv_int(ptr, BOOT_PARAM_ID_ACTIVITY_MOVE_CONFIG,
			    CONFIG_ALIF_PM_ACTIVITY_MOVE_CONFIG,
			    BOOT_PARAM_LEN_ACTIVITY_MOVE_CONFIG);
	ptr = write_tlv_int(ptr, BOOT_PARAM_ID_SCAN_EXT_ADV, CONFIG_ALIF_PM_SCAN_EXT_ADV,
			    BOOT_PARAM_LEN_SCAN_EXT_ADV);
	ptr = write_tlv_int(ptr, BOOT_PARAM_ID_RSSI_HIGH_THR, CONFIG_ALIF_PM_RSSI_HIGH_THR,
			    BOOT_PARAM_LEN_RSSI_THR);
	ptr = write_tlv_int(ptr, BOOT_PARAM_ID_RSSI_LOW_THR, CONFIG_ALIF_PM_RSSI_LOW_THR,
			    BOOT_PARAM_LEN_RSSI_THR);
	ptr = write_tlv_int(ptr, BOOT_PARAM_ID_SLEEP_ENABLE, CONFIG_ALIF_PM_SLEEP_ENABLE,
			    BOOT_PARAM_LEN_SLEEP_ENABLE);
	ptr = write_tlv_int(ptr, BOOT_PARAM_ID_EXT_WAKEUP_ENABLE, CONFIG_ALIF_PM_EXT_WAKEUP_ENABLE,
			    BOOT_PARAM_LEN_EXT_WAKEUP_ENABLE);
	ptr = write_tlv_int(ptr, BOOT_PARAM_ID_ENABLE_CHANNEL_ASSESSMENT,
			    CONFIG_ALIF_PM_ENABLE_CH_ASSESSMENT,
			    BOOT_PARAM_LEN_ENABLE_CHANNEL_ASSESSMENT);
	ptr = write_tlv_int(ptr, BOOT_PARAM_ID_RSSI_INTERF_THR, CONFIG_ALIF_PM_RSSI_INTERF_THR,
			    BOOT_PARAM_LEN_RSSI_THR);
	ptr = write_tlv_int(ptr, BOOT_PARAM_ID_UART_BAUDRATE, used_baudrate,
			    BOOT_PARAM_LEN_UART_BAUDRATE);
	ptr = write_tlv_int(ptr, BOOT_PARAM_ID_EXT_WAKEUP_TIME, CONFIG_ALIF_EXT_WAKEUP_TIME,
			    BOOT_PARAM_LEN_EXT_WAKEUP_TIME);
	ptr = write_tlv_int(ptr, BOOT_PARAM_ID_OSC_WAKEUP_TIME, CONFIG_ALIF_OSC_WAKEUP_TIME,
			    BOOT_PARAM_LEN_OSC_WAKEUP_TIME);
	ptr = write_tlv_int(ptr, BOOT_PARAM_ID_RM_WAKEUP_TIME, CONFIG_ALIF_RM_WAKEUP_TIME,
			    BOOT_PARAM_LEN_RM_WAKEUP_TIME);
	ptr = write_tlv_int(ptr, BOOT_PARAM_ID_EXT_WARMBOOT_WAKEUP_TIME,
			    CONFIG_ALIF_EXT_WARMBOOT_WAKEUP_TIME,
			    BOOT_PARAM_LEN_EXT_WARMBOOT_WAKEUP_TIME);
	ptr = write_tlv_int(ptr, BOOT_PARAM_ID_LPCLK_DRIFT, CONFIG_ALIF_MAX_SLEEP_CLOCK_DRIFT,
			    BOOT_PARAM_LEN_LPCLK_DRIFT);
	ptr = write_tlv_int(ptr, BOOT_PARAM_ID_ACTCLK_DRIFT, CONFIG_ALIF_MAX_ACTIVE_CLOCK_DRIFT,
			    BOOT_PARAM_LEN_ACTCLK_DRIFT);
	ptr = write_tlv_int(ptr, BOOT_PARAM_ID_CONFIGURATION, config,
			    BOOT_PARAM_LEN_CONFIGURATION);

	/* UART input clock can be configured as 16/24/48Mhz */
	if (min_uart_clk_freq > 16000000) {
		if (min_uart_clk_freq <= 24000000) {
			ll_uart_clk_freq = 24000000;
			reg_uart_clk_cfg = LL_UART_CLK_SEL_CTRL_24MHZ;
		} else {
			ll_uart_clk_freq = 48000000;
			reg_uart_clk_cfg = LL_UART_CLK_SEL_CTRL_48MHZ;
		}
	}
	/* Add UART clock select */
	es0_clock_select |= reg_uart_clk_cfg;
	ptr = write_tlv_int(ptr, BOOT_PARAM_ID_UART_INPUT_CLK_FREQ, ll_uart_clk_freq,
			    BOOT_PARAM_LEN_UART_INPUT_CLK_FREQ);

	total_length = ptr - ll_boot_params_buffer;

	if (total_length < (NVD_BOOT_PARAMS_MAX_SIZE - 2)) {
		ptr = write_tlv_int(ptr, BOOT_PARAM_ID_NO_PARAM, 0, 0);
		total_length += 3;
	}

	int8_t ret = take_es0_into_use_with_params(ll_boot_params_buffer, total_length,
						   es0_clock_select);

	shell_fprintf(shell, SHELL_VT100_COLOR_DEFAULT, "Start ES0 ret:%d\n", ret);
	return 0;
}

static int cmd_stop(const struct shell *shell, size_t argc, char **argv)
{
	int8_t ret = stop_using_es0();

	shell_fprintf(shell, SHELL_VT100_COLOR_DEFAULT, "Stop ES0 ret:%d\n", ret);
	return 0;
}
static int cmd_uart_wiggle(const struct shell *shell, size_t argc, char **argv)
{
	/*
	 * sys_write32(0x10000, 0x4903F00C);
	 * sys_set_bits(M55HE_CFG_HE_CLK_ENA, BIT(8));
	 * data =  sys_read32(0x1A602008);
	 */

	shell_fprintf(shell, SHELL_VT100_COLOR_DEFAULT, "set HCI uart flowcontrol to manual\n");
	sys_write8(0x00, 0x4300A010);
	sys_write8(0x02, 0x4300A010);
	sys_write8(0x00, 0x4300A010);
	shell_fprintf(shell, SHELL_VT100_COLOR_DEFAULT, "set HCI uart flowcontrol to automatic\n");
	sys_write8(0x2b, 0x4300A010);
	return 0;
}

static int cmd_uart_wakeup(const struct shell *shell, size_t argc, char **argv)
{
	shell_fprintf(shell, SHELL_VT100_COLOR_DEFAULT, "set HCI uart flowcontrol to automatic\n");
	sys_write8(0x2b, 0x4300A010);
	return 0;
}

static int cmd_uart_sleep(const struct shell *shell, size_t argc, char **argv)
{
	shell_fprintf(shell, SHELL_VT100_COLOR_DEFAULT,
		      "set HCI uart flowcontrol to manual and sleep\n");
	sys_write8(0x00, 0x4300A010);
	return 0;
}

static int cmd_uart(const struct shell *shell, size_t argc, char **argv)
{

	if (param_get_flag(argc, argv, "--wiggle")) {
		cmd_uart_wiggle(shell, argc, argv);
		return 0;
	}
	if (param_get_flag(argc, argv, "--sleep")) {
		cmd_uart_sleep(shell, argc, argv);
		return 0;
	}
	cmd_uart_wakeup(shell, argc, argv);
	return 0;
}

const pinctrl_soc_pin_t pinctrl_hci_a_ext[] = {PIN_P3_6__EXT_RTS_A | 0x10000, /*Receiver enabled*/
					       PIN_P3_7__EXT_CTS_A | 0x10000, /*Receiver enabled*/
					       PIN_P4_0__EXT_RX_A | 0x10000,  /*Receiver enabled*/
					       PIN_P4_1__EXT_TX_A | 0x10000,  /*Receiver enabled*/
					       PIN_P4_2__EXT_TRACE_A};

const pinctrl_soc_pin_t pinctrl_hci_b_ext[] = {PIN_P8_3__EXT_RTS_B | 0x10000, /*Receiver enabled*/
					       PIN_P8_4__EXT_CTS_B | 0x10000, /*Receiver enabled*/
					       PIN_P8_6__EXT_RX_B | 0x10000,  /*Receiver enabled*/
					       PIN_P8_7__EXT_TX_B | 0x10000,  /*Receiver enabled*/
					       PIN_P8_5__EXT_TRACE_B};

static int cmd_hci(const struct shell *shell, size_t argc, char **argv)
{

	/*
	 * 0x1a60_5008.
	 * AHI select register is bit 0
	 * HCI select register is bit 1
	 * AHI/HCI trace select register is bit 2.
	 */
	uint32_t trace_select = 0x02;

	if (param_get_flag(argc, argv, "--ahi")) {
		trace_select = 0x01;
	}
	if (param_get_flag(argc, argv, "--trace")) {
		trace_select |= 0x04;
	}

	if (param_get_flag(argc, argv, "--pinmux_b")) {
		pinctrl_configure_pins(pinctrl_hci_b_ext, ARRAY_SIZE(pinctrl_hci_b_ext),
				       PINCTRL_REG_NONE);
	} else {
		pinctrl_configure_pins(pinctrl_hci_a_ext, ARRAY_SIZE(pinctrl_hci_a_ext),
				       PINCTRL_REG_NONE);
	}
	shell_fprintf(shell, SHELL_VT100_COLOR_DEFAULT,
		      "configuring external UART trace select:0x%x\n", trace_select);

	sys_write32(trace_select, 0x1a605008);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_cmds, SHELL_CMD_ARG(start, NULL, "es0 start", cmd_start, 1, 10),
#if defined(CONFIG_PM)
	SHELL_CMD_ARG(pm_off, NULL, "Start Off-state sequency --period --cnt", cmd_off_test, 1, 10),
	SHELL_CMD_ARG(pm_standby, NULL, "Start Standby-state sequency  --period", cmd_standby_test,
		      1, 10),
	SHELL_CMD_ARG(pm_idle, NULL, "Start Standby-state sequency  --period", cmd_idle_test, 1,
		      10),
#endif
	SHELL_CMD_ARG(stop, NULL, "es0 stop", cmd_stop, 1, 10),
	SHELL_CMD_ARG(uart, NULL, "es0 uart wakeup --sleep --wiggle", cmd_uart, 1, 10),
	SHELL_CMD_ARG(hci, NULL, "Configure ext HCI: --ahi --trace --pinmux_b", cmd_hci, 1, 10),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(pwr, &sub_cmds, "Power management test commands", NULL);
