/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef _DRIVER_WM8904_H
#define _DRIVER_WM8904_H

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/audio/codec.h>

/*
 * R20 (0x14) - Clock Rates 0
 */
#define CLK_RTE0_SYSCLK_SRC_Pos  10 /* SYSCLK_SRC */
#define CLK_RTE0_SYSCLK_SRC_Msk  0x0C00
#define CLK_RTE0_SYSCLK_SRC_FLL  (0b10 << CLK_RTE0_SYSCLK_SRC_Pos)
#define CLK_RTE0_SYSCLK_SRC_MCLK (0b01 << CLK_RTE0_SYSCLK_SRC_Pos)
#define CLK_RTE0_SYSCLK_SRC_OSC  (0b00 << CLK_RTE0_SYSCLK_SRC_Pos)

#define CLK_RTE0_MCLK_DIV_Pos 1 /* MCLK_DIV */
#define CLK_RTE0_MCLK_DIV_Msk 0x0006
#define CLK_RTE0_MCLK_DIV_6   (0b11 << CLK_RTE0_MCLK_DIV_Pos)
#define CLK_RTE0_MCLK_DIV_3   (0b10 << CLK_RTE0_MCLK_DIV_Pos)
#define CLK_RTE0_MCLK_DIV_1_5 (0b01 << CLK_RTE0_MCLK_DIV_Pos)
#define CLK_RTE0_MCLK_DIV_1   (0b00 << CLK_RTE0_MCLK_DIV_Pos)

/* Driver configuration */
struct wm8904_driver_config {
	struct i2c_dt_spec i2c;
	uint8_t clock_source;
	const struct device *mclk_dev;
	clock_control_subsys_t mclk_name;
};

#define WM8904_DEV_ID 0x8904

/*
 * WM8904_Quick_startup_sequence_register
 */
#define WM8904_SW_RESET_AND_ID   0x00
#define WM8904_WRITE_SEQUENCER_0 0x6C
#define WM8904_WRITE_SEQUENCER_3 0x6F
#define WM8094_WRITE_SEQUENCER_4 0x70

/*
 * WM8904_Start-up sequence Register 1
 */
#define WM8904_BIAS_CONTROL_0     0x04
#define WM8904_VMID_CONTROL_0     0x05
#define WM8904_VMID_CONTROL_0     0x05
#define WM8904_BIAS_CONTROL_0     0x04
#define WM8904_POWER_MANAGEMENT_0 0x0C
#define WM8904_POWER_MANAGEMENT_2 0x0E
#define WM8904_POWER_MANAGEMENT_3 0x0F
#define WM8904_POWER_MANAGEMENT_6 0x12
#define WM8904_CLOCK_RATES_0      0x14
#define WM8904_CLOCK_RATES_1      0x15
#define WM8904_CLOCK_RATES_2      0x16

/*
 * WM8904_Start-up sequence Register 2
 */
#define WM8904_BIAS_CONTROL_0 0x04
#define WM8904_CHARGE_PUMP_0  0x62

/*
 * WM8904_Start-up sequence Register 3
 */
#define WM8904_ANALOGUE_HP_0      0x5A
#define WM8904_ANALOGUE_LINEOUT_0 0x5E
#define WM8904_ANALOGUE_HP_0      0x5A
#define WM8904_ANALOGUE_LINEOUT_0 0x5E
#define WM8904_DC_SERVO_0         0x43
#define WM8904_DC_SERVO_1         0x44

/*
 * WM8904_Start-up sequence Register 4
 */
#define WM8904_ANALOGUE_HP_0      0x5A
#define WM8904_ANALOGUE_LINEOUT_0 0x5E
#define WM8904_ANALOGUE_HP_0      0x5A
#define WM8904_ANALOGUE_LINEOUT_0 0x5E

/*
 * WM8904_control_register
 */
#define WM8904_ANALOGUE_OUT12_ZC        0x3D
#define WM8904_ANALOGUE_LEFT_INPUT_0    0x2C
#define WM8904_ANALOGUE_LEFT_INPUT_1    0x2E
#define WM8904_ANALOGUE_RIGHT_INPUT_0   0x2D
#define WM8904_ANALOGUE_RIGHT_INPUT_1   0x2F
#define WM8904_DAC_DIGITAL_1            0x21
#define WM8904_CLASS_W_0                0x68
#define WM8904_DIGITAL_PULLS            0x7E
#define WM8904_DAC_DIGITAL_VOLUME_LEFT  0x1E
#define WM8904_DAC_DIGITAL_VOLUME_RIGHT 0x1F
#define WM8904_DRC_0                    0x28
#define WM8904_AUDIO_INTERFACE_0        0x18
#define WM8904_AUDIO_INTERFACE_1        0x19
#define WM8904_ANALOGUE_OUT1_LEFT       0x39
#define WM8904_ANALOGUE_OUT1_RIGHT      0x3A

/*
 * WM8904 FLL (frequency locked loop) registers
 */
#define WM8904_FLL_CONTROL_1 0x74
#define WM8904_FLL_CONTROL_2 0x75
#define WM8904_FLL_CONTROL_3 0x76
#define WM8904_FLL_CONTROL_4 0x77
#define WM8904_FLL_CONTROL_5 0x78

/*
 * WM8904 Interrupt registers
 */
#define WM8904_INTERRUPT_STATUS 0x7F

/* REGISTERS MASKING: */

/*
 * R4 (0x04) - Bias Control 0
 */

#define BIAS_CNTL_ISEL_Pos     2 /* ISEL - [3:2] */
#define BIAS_CNTL_ISEL_Msk     0x0008
#define BIAS_CNTL_ISEL(Value)  (BIAS_CNTL_ISEL_Msk & ((Value) << BIAS_CNTL_ISEL_Pos))
#define BIAS_CNTL_ISEL_LP_BIAS BIAS_CNTL_ISEL(0)
#define BIAS_CNTL_ISEL_HP_BIAS 0x0008 /* BIAS_CNTL_ISEL(2) */
#define BIAS_CNTL_BIAS_ENA_Pos 0      /* BIAS_ENA */
#define BIAS_CNTL_BIAS_ENA     0x0001

/*
 * R5 (0x05) - VMID Control 0
 */
#define VMID_CNTL0_VMID_BUF_ENA_Pos 6 /* VMID_BUF_ENA */
#define VMID_CNTL0_VMID_BUF_ENA     0x0040
#define VMID_CNTL0_VMID_RES_Pos     1 /* VMID_RES - [2:1] */
#define VMID_CNTL0_VMID_RES_Msk     0x0006
#define VMID_CNTL0_VMID_RES(Value)  (VMID_CNTL0_VMID_RES_Msk & ((Value) << VMID_CNTL0_VMID_RES_Pos))
#define VMID_CNTL0_VMID_RES_OFF     VMID_CNTL0_VMID_RES(0)
#define VMID_CNTL0_VMID_RES_NORMAL  VMID_CNTL0_VMID_RES(1)
#define VMID_CNTL0_VMID_RES_LP      VMID_CNTL0_VMID_RES(2)
#define VMID_CNTL0_VMID_RES_FAST    0x0006 /* VMID_CNTL0_VMID_RES(3) */
#define VMID_CNTL0_VMID_ENA_Pos     0      /* VMID_ENA */
#define VMID_CNTL0_VMID_ENA         0x0001

/*
 * R12 (0x0C) - Power Management 0
 */
#define PWR_MGMT0_INL_ENA_Pos 1 /* INL_ENA */
#define PWR_MGMT0_INL_ENA     0x0002
#define PWR_MGMT0_INR_ENA_Pos 0 /* INR_ENA */
#define PWR_MGMT0_INR_ENA     0x0001

/*
 * R14 (0x0E) - Power Management 2
 */
#define PWR_MGMT2_HPL_PGA_ENA_Pos 1 /* HPL_PGA_ENA */
#define PWR_MGMT2_HPL_PGA_ENA     0x0002
#define PWR_MGMT2_HPR_PGA_ENA_Pos 0 /* HPR_PGA_ENA */
#define PWR_MGMT2_HPR_PGA_ENA     0x0001

/*
 * R15 (0x0F) - Power Management 3
 */
#define PWR_MGMT3_LINEOUTL_PGA_ENA_Pos 1 /* LINEOUTL_PGA_ENA */
#define PWR_MGMT3_LINEOUTL_PGA_ENA     0x0002
#define PWR_MGMT3_LINEOUTR_PGA_ENA_Pos 0 /* LINEOUTR_PGA_ENA */
#define PWR_MGMT3_LINEOUTR_PGA_ENA     0x0001

/*
 * R21 (0x15) - Clock Rates 1
 */
#define CLK_RATE1_SYS_RATE_128    (0b0001 << 10)
#define CLK_RATE1_SYS_RATE_256    (0b0011 << 10)
#define CLK_RATE1_SAMPLE_RATE_48K (0b101)

/*
 * R22 (0x16) - Clock Rates 2
 */
#define CLK_RTE2_SYSCLK_SRC_Pos  14 /* SYSCLK_SRC */
#define CLK_RTE2_SYSCLK_SRC      0x4000
#define CLK_RTE2_TO_CLK_RATE_Pos 12 /* TO_CLK_RATE */
#define CLK_RTE2_TO_CLK_RATE     0x1000
#define CLK_RTE2_CLK_SYS_ENA_Pos 2 /* CLK_SYS_ENA */
#define CLK_RTE2_CLK_SYS_ENA     0x0004
#define CLK_RTE2_CLK_DSP_ENA_Pos 1 /* CLK_DSP_ENA */
#define CLK_RTE2_CLK_DSP_ENA     0x0002

/*
 * R30 (0x1E) - DAC Digital Volume Left
 */
#define DAC_DIG_VOL_DAC_VU_Pos   8 /* DAC_VU */
#define DAC_DIG_VOL_DAC_VU       0x0100
#define DAC_DIG_VOL_DACL_VOL_Pos 0      /* DACL_VOL - [7:0] */
#define DAC_DIG_VOL_DACL_VOL_Msk 0x00FF /* DACL_VOL - [7:0] */
#define DAC_DIG_VOL_DACL_VOL(Value)                                                                \
	(DAC_DIG_VOL_DACL_VOL_Msk & ((Value) << DAC_DIG_VOL_DACL_VOL_Pos))

/*
 * R31 (0x1F) - DAC Digital Volume Right
 */
#define DAC_DIG_VOL_DAC_VU_Pos   8 /* DAC_VU */
#define DAC_DIG_VOL_DAC_VU       0x0100
#define DAC_DIG_VOL_DACR_VOL_Pos 0 /* DACR_VOL - [7:0] */
#define DAC_DIG_VOL_DACR_VOL_Msk 0x00FF
#define DAC_DIG_VOL_DACR_VOL(Value)                                                                \
	(DAC_DIG_VOL_DACR_VOL_Msk & ((Value) << DAC_DIG_VOL_DACR_VOL_Pos))

/*
 * R18 (0x12) - Power Management 6
 */
#define PWR_MGMT6_DACL_ENA_Pos 3 /* DACL_ENA */
#define PWR_MGMT6_DACL_ENA     0x0008
#define PWR_MGMT6_DACR_ENA_Pos 2 /* DACR_ENA */
#define PWR_MGMT6_DACR_ENA     0x0004
#define PWR_MGMT6_ADCL_ENA_Pos 1 /* ADCL_ENA */
#define PWR_MGMT6_ADCL_ENA     0x0002
#define PWR_MGMT6_ADCR_ENA_Pos 0 /* ADCR_ENA */
#define PWR_MGMT6_ADCR_ENA     0x0001

/*
 * R98 (0x62) - Charge Pump 0
 */
#define CHRG_PMP_CP_ENA_Pos 0 /* CP_ENA */
#define CHRG_PMP_CP_ENA     0x0001

/*
 * R90 (0x5A) - Analogue HP 0
 */
#define ANLG_HP0_HPL_RMV_SHORT_Pos 7 /* HPL_RMV_SHORT */
#define ANLG_HP0_HPL_RMV_SHORT     0x0080
#define ANLG_HP0_HPL_ENA_OUTP_Pos  6 /* HPL_ENA_OUTP */
#define ANLG_HP0_HPL_ENA_OUTP      0x0040
#define ANLG_HP0_HPL_ENA_DLY_Pos   5 /* HPL_ENA_DLY */
#define ANLG_HP0_HPL_ENA_DLY       0x0020
#define ANLG_HP0_HPL_ENA_Pos       4 /* HPL_ENA */
#define ANLG_HP0_HPL_ENA           0x0010
#define ANLG_HP0_HPR_RMV_SHORT_Pos 3 /* HPR_RMV_SHORT */
#define ANLG_HP0_HPR_RMV_SHORT     0x0008
#define ANLG_HP0_HPR_ENA_OUTP_Pos  2 /* HPR_ENA_OUTP */
#define ANLG_HP0_HPR_ENA_OUTP      0x0004
#define ANLG_HP0_HPR_ENA_DLY_Pos   1 /* HPR_ENA_DLY */
#define ANLG_HP0_HPR_ENA_DLY       0x0002
#define ANLG_HP0_HPR_ENA_Pos       0 /* HPR_ENA */
#define ANLG_HP0_HPR_ENA           0x0001

/*
 * R94 (0x5E) - Analogue Lineout 0
 */
#define ANLG_LINOUT0_LINEOUTL_RMV_SHORT_Pos 7 /* LINEOUTL_RMV_SHORT */
#define ANLG_LINOUT0_LINEOUTL_RMV_SHORT     0x0080
#define ANLG_LINOUT0_LINEOUTL_ENA_OUTP_Pos  6 /* LINEOUTL_ENA_OUTP */
#define ANLG_LINOUT0_LINEOUTL_ENA_OUTP      0x0040
#define ANLG_LINOUT0_LINEOUTL_ENA_DLY_Pos   5 /* LINEOUTL_ENA_DLY */
#define ANLG_LINOUT0_LINEOUTL_ENA_DLY       0x0020
#define ANLG_LINOUT0_LINEOUTL_ENA_Pos       4 /* LINEOUTL_ENA */
#define ANLG_LINOUT0_LINEOUTL_ENA           0x0010
#define ANLG_LINOUT0_LINEOUTR_RMV_SHORT_Pos 3 /* LINEOUTR_RMV_SHORT */
#define ANLG_LINOUT0_LINEOUTR_RMV_SHORT     0x0008
#define ANLG_LINOUT0_LINEOUTR_ENA_OUTP_Pos  2 /* LINEOUTR_ENA_OUTP */
#define ANLG_LINOUT0_LINEOUTR_ENA_OUTP      0x0004
#define ANLG_LINOUT0_LINEOUTR_ENA_DLY_Pos   1 /* LINEOUTR_ENA_DLY */
#define ANLG_LINOUT0_LINEOUTR_ENA_DLY       0x0002
#define ANLG_LINOUT0_LINEOUTR_ENA_Pos       0 /* LINEOUTR_ENA */
#define ANLG_LINOUT0_LINEOUTR_ENA           0x0001

/*
 * R67 (0x43) - DC Servo 0
 */
#define DC_SRV0_DCS_ENA_CHAN_3_Pos 3 /* DCS_ENA_CHAN_3 */
#define DC_SRV0_DCS_ENA_CHAN_3     0x0008
#define DC_SRV0_DCS_ENA_CHAN_2_Pos 2 /* DCS_ENA_CHAN_2 */
#define DC_SRV0_DCS_ENA_CHAN_2     0x0004
#define DC_SRV0_DCS_ENA_CHAN_1_Pos 1 /* DCS_ENA_CHAN_1 */
#define DC_SRV0_DCS_ENA_CHAN_1     0x0002
#define DC_SRV0_DCS_ENA_CHAN_0_Pos 0 /* DCS_ENA_CHAN_0 */
#define DC_SRV0_DCS_ENA_CHAN_0     0x0001

/*
 * R68 (0x44) - DC Servo 1
 */
#define DC_SRV1_DCS_TRIG_STARTUP_3_Pos 7 /* DCS_TRIG_STARTUP_3 */
#define DC_SRV1_DCS_TRIG_STARTUP_3     0x0080
#define DC_SRV1_DCS_TRIG_STARTUP_2_Pos 6 /* DCS_TRIG_STARTUP_2 */
#define DC_SRV1_DCS_TRIG_STARTUP_2     0x0040
#define DC_SRV1_DCS_TRIG_STARTUP_1_Pos 5 /* DCS_TRIG_STARTUP_1 */
#define DC_SRV1_DCS_TRIG_STARTUP_1     0x0020
#define DC_SRV1_DCS_TRIG_STARTUP_0_Pos 4 /* DCS_TRIG_STARTUP_0 */
#define DC_SRV1_DCS_TRIG_STARTUP_0     0x0010

/*
 * R108 (0x6C) - Write Sequencer 0
 */
#define WTE_SEQ0_WSEQ_ENA_Pos 8 /* WSEQ_ENA */
#define WTE_SEQ0_WSEQ_ENA     0x0100

/*
 * R111 (0x6F) - Write Sequencer 3
 */
#define WTE_SEQ3_WSEQ_START_Pos 8 /* WSEQ_START */
#define WTE_SEQ3_WSEQ_START     0x0100

/*
 * R112 (0x70) - Write Sequencer 4
 */
#define WTE_SEQ4_WSEQ_BUSY_Bit 0x0001

/*
 * R61 (0x3D) - Analogue OUT12 ZC
 */
#define ANLG_OUT12ZC_HPL_BYP_ENA_Pos 3 /* HPL_BYP_ENA */
#define ANLG_OUT12ZC_HPL_BYP_ENA     0x0008
#define ANLG_OUT12ZC_HPR_BYP_ENA_Pos 2 /* HPR_BYP_ENA */
#define ANLG_OUT12ZC_HPR_BYP_ENA     0x0004

/*
 * R44 (0x2C) - Analogue Left Input 0
 */
#define ANLG_LIN0_LINMUTE_Pos 7 /* LINMUTE */
#define ANLG_LIN0_LINMUTE     0x0080
#define ANLG_LIN0_VOL_Pos     0 /* LIN_VOL - [4:0] */
#define ANLG_LIN0_VOL_Msk     0x001F
#define ANLG_LIN0_VOL(Value)  (ANLG_LIN0_VOL_Msk & ((Value) << ANLG_LIN0_VOL_Pos))

/*
 * R45 (0x2D) - Analogue Right Input 0
 */
#define ANLG_RIN0_RINMUTE_Pos 7 /* RINMUTE */
#define ANLG_RIN0_RINMUTE     0x0080
#define ANLG_RIN0_VOL_Pos     0 /* RIN_VOL - [4:0] */
#define ANLG_RIN0_VOL_Msk     0x001F
#define ANLG_RIN0_VOL(Value)  (ANLG_RIN0_VOL_Msk & ((Value) << ANLG_RIN0_VOL_Pos))

/*
 * R57 (0x39) - Analogue OUT1 Left
 */
#define ANLG_OUT1_HPOUTL_MUTE_Pos 8 /* HPOUTL_MUTE */
#define ANLG_OUT1_HPOUTL_MUTE     0x0100
#define ANLG_OUT1_HPOUTL_VU_Pos   7 /* HPOUTL_VU */
#define ANLG_OUT1_HPOUTL_VU       0x0080
#define ANLG_OUT1_HPOUTLZC_Pos    6 /* HPOUTLZC */
#define ANLG_OUT1_HPOUTLZC        0x0040
#define ANLG_OUT1_HPOUTL_VOL_Pos  0 /* HPOUTL_VOL - [5:0] */
#define ANLG_OUT1_HPOUTL_VOL_Msk  0x003F
#define ANLG_OUT1_HPOUTL_VOL(Value)                                                                \
	(ANLG_OUT1_HPOUTL_VOL_Msk & ((Value) << ANLG_OUT1_HPOUTL_VOL_Pos))

/*
 * R58 (0x3A) - Analogue OUT1 Right
 */
#define ANLG_OUT1_HPOUTR_MUTE_Pos 8 /* HPOUTR_MUTE */
#define ANLG_OUT1_HPOUTR_MUTE     0x0100
#define ANLG_OUT1_HPOUTR_VU_Pos   7 /* HPOUTR_VU */
#define ANLG_OUT1_HPOUTR_VU       0x0080
#define ANLG_OUT1_HPOUTRZC_Pos    6 /* HPOUTRZC */
#define ANLG_OUT1_HPOUTRZC        0x0040
#define ANLG_OUT1_HPOUTR_VOL_Pos  0 /* HPOUTR_VOL - [5:0] */
#define ANLG_OUT1_HPOUTR_VOL_Msk  0x003F
#define ANLG_OUT1_HPOUTR_VOL(Value)                                                                \
	(ANLG_OUT1_HPOUTR_VOL_Msk & ((Value) << ANLG_OUT1_HPOUTR_VOL_Pos))

/*
 * R46 (0x2E) - Analogue Left Input 1
 */
#define ANLG_LIN1_INL_CM_ENA_Pos    6 /* INL_CM_ENA */
#define ANLG_LIN1_INL_CM_ENA        0x0040
#define ANLG_LIN1_IP_SEL_N_Pos      4 /* L_IP_SEL_N - [5:4] */
#define ANLG_LIN1_IP_SEL_N_Msk      0x0030
#define ANLG_LIN1_IP_SEL_N(Value)   (ANLG_LIN1_IP_SEL_N_Msk & ((Value) << ANLG_LIN1_IP_SEL_N_Pos))
#define ANLG_LIN1_IP_SEL_N_IN1L     ANLG_LIN1_IP_SEL_N(0)
#define ANLG_LIN1_IP_SEL_N_IN2L     ANLG_LIN1_IP_SEL_N(1)
#define ANLG_LIN1_IP_SEL_N_IN3L     ANLG_LIN1_IP_SEL_N(2)
#define ANLG_LIN1_IP_SEL_P_Pos      2 /* L_IP_SEL_P - [3:2] */
#define ANLG_LIN1_IP_SEL_P_Msk      0x000C
#define ANLG_LIN1_IP_SEL_P(Value)   (ANLG_LIN1_IP_SEL_P_Msk & ((Value) << ANLG_LIN1_IP_SEL_P_Pos))
#define ANLG_LIN1_IP_SEL_P_IN1L     ANLG_LIN1_IP_SEL_P(0)
#define ANLG_LIN1_IP_SEL_P_IN2L     ANLG_LIN1_IP_SEL_P(1)
#define ANLG_LIN1_IP_SEL_P_IN3L     ANLG_LIN1_IP_SEL_P(2)
#define ANLG_LIN1_MODE_Pos          0 /* L_MODE - [1:0] */
#define ANLG_LIN1_MODE_Msk          0x0003
#define ANLG_LIN1_MODE(Value)       (ANLG_LIN1_MODE_Msk & ((Value) << ANLG_LIN1_MODE_Pos))
#define ANLG_LIN1_MODE_SINGLE_ENDED ANLG_LIN1_MODE(0)
#define ANLG_LIN1_MODE_DIFF_LINE    ANLG_LIN1_MODE(1)
#define ANLG_LIN1_MODE_DIFF_MIC     WM8904_L_MODE(2)

/*
 * R47 (0x2F) - Analogue Right Input 1
 */
#define ANLG_RIN1_INR_CM_ENA_Pos    6 /* INR_CM_ENA */
#define ANLG_RIN1_INR_CM_ENA        0x0040
#define ANLG_RIN1_IP_SEL_N_Pos      4 /* R_IP_SEL_N - [5:4] */
#define ANLG_RIN1_IP_SEL_N_Msk      0x0030
#define ANLG_RIN1_IP_SEL_N(Value)   (ANLG_RIN1_IP_SEL_N_Msk & ((Value) << ANLG_RIN1_IP_SEL_N_Pos))
#define ANLG_RIN1_IP_SEL_N_IN1R     ANLG_RIN1_IP_SEL_N(0)
#define ANLG_RIN1_IP_SEL_N_IN2R     ANLG_RIN1_IP_SEL_N(1)
#define ANLG_RIN1_IP_SEL_N_IN3R     ANLG_RIN1_IP_SEL_N(2)
#define ANLG_RIN1_IP_SEL_P_Pos      2 /* R_IP_SEL_P - [3:2] */
#define ANLG_RIN1_IP_SEL_P_Msk      0x000C
#define ANLG_RIN1_IP_SEL_P(Value)   (ANLG_RIN1_IP_SEL_P_Msk & ((Value) << ANLG_RIN1_IP_SEL_P_Pos))
#define ANLG_RIN1_IP_SEL_P_IN1R     ANLG_RIN1_IP_SEL_P(0)
#define ANLG_RIN1_IP_SEL_P_IN2R     ANLG_RIN1_IP_SEL_P(1)
#define ANLG_RIN1_IP_SEL_P_IN3R     ANLG_RIN1_IP_SEL_P(2)
#define ANLG_RIN1_MODE_Pos          0 /* R_MODE - [1:0] */
#define ANLG_RIN1_MODE_Msk          0x0003
#define ANLG_RIN1_MODE(Value)       (ANLG_RIN1_MODE_Msk & ((Value) << ANLG_RIN1_MODE_Pos))
#define ANLG_RIN1_MODE_SINGLE_ENDED ANLG_RIN1_MODE(0)
#define ANLG_RIN1_MODE_DIFF_LINE    ANLG_RIN1_MODE(1)
#define ANLG_RIN1_MODE_DIFF_MIC     ANLG_RIN1_MODE(2)

/*
 * R33 (0x21) - DAC Digital 1
 */
#define DAC_DG1_MONO_Pos        12 /* DAC_MONO */
#define DAC_DG1_MONO            0x1000
#define DAC_DG1_SB_FILT_Pos     11 /* DAC_SB_FILT */
#define DAC_DG1_SB_FILT         0x0800
#define DAC_DG1_MUTERATE_Pos    10 /* DAC_MUTERATE */
#define DAC_DG1_MUTERATE        0x0400
#define DAC_DG1_UNMUTE_RAMP_Pos 9 /* DAC_UNMUTE_RAMP */
#define DAC_DG1_UNMUTE_RAMP     0x0200
#define DAC_DG1_OSR128_Pos      6 /* DAC_OSR128 */
#define DAC_DG1_OSR128          0x0040
#define DAC_DG1_MUTE_Pos        3 /* DAC_MUTE */
#define DAC_DG1_MUTE            0x0008
#define DAC_DG1_DEEMPH_Pos      1 /* DEEMPH - [2:1] */
#define DAC_DG1_DEEMPH_Msk      0x0006
#define DAC_DG1_DEEMPH(Value)   (DAC_DG1_DEEMPH_Msk & ((Value) << DAC_DG1_DEEMPH_Pos))

/*
 * R104 (0x68) - Class W 0
 */
#define CLS_W0_CP_DYN_PWR_Pos 0 /* CP_DYN_PWR */
#define CLS_W0_CP_DYN_PWR     0x0001

/*
 * R126 (0x7E) - Digital Pulls
 */
#define DG_PLS_LRCLK_PU_Pos 3 /* LRCLK_PU */
#define DG_PLS_LRCLK_PU     0x0008
#define DG_PLS_BCLK_PU_Pos  1 /* BCLK_PU */
#define DG_PLS_BCLK_PU      0x0002

/*
 * R30 (0x1E) - DAC Digital Volume Left
 */
#define DAC_DG_VL_DACL_VOL_Pos     0      /* DACL_VOL - [7:0] */
#define DAC_DG_VL_DACL_VOL         0x008A /* DACL_VOL - [7:0] */
#define DAC_DG_VL_DACL_VOL1(Value) (DAC_DG_VL_DACL_VOL & ((Value) << DAC_DG_VL_DACL_VOL_Pos))

/*
 * R31 (0x1F) - DAC Digital Volume Right
 */
#define DAC_DG_VR_DACR_VOL_Pos     0 /* DACR_VOL - [7:0] */
#define DAC_DG_VR_DACR_VOL         0x008A
#define DAC_DG_VR_DACR_VOL1(Value) (DAC_DG_VR_DACR_VOL & ((Value) << DAC_DG_VR_DACR_VOL_Pos))

/*
 * R40 (0x28) - DRC 0
 */
#define DRC0_DRC_ENA_Pos      15 /* DRC_ENA */
#define DRC0_DRC_ENA          0x8000
#define DRC0_DRC_DAC_PATH_Pos 14 /* DRC_DAC_PATH */
#define DRC0_DRC_DAC_PATH     0x4000

/*
 * R25 (0x19) - Audio Interface 1
 */
#define AUD_INT1_AIF_WL_Pos     2 /* AIF_WL - [3:2] */
#define AUD_INT1_AIF_WL_Msk     0x0008
#define AUD_INT1_AIF_WL(Value)  (AUD_INT1_AIF_WL_Msk & ((Value) << AUD_INT1_AIF_WL_Pos))
#define AUD_INT1_AIF_WL_16BIT   AUD_INT1_AIF_WL(0)
#define AUD_INT1_AIF_WL_20BIT   AUD_INT1_AIF_WL(1)
#define AUD_INT1_AIF_WL_24BIT   0x0008 /* AUD_INT1_AIF_WL(2) */
#define AUD_INT1_AIF_WL_32BIT   AUD_INT1_AIF_WL(3)
#define AUD_INT1_AIF_FMT_Pos    0 /* AIF_FMT - [1:0] */
#define AUD_INT1_AIF_FMT_Msk    0x0002
#define AUD_INT1_AIF_FMT(Value) (AUD_INT1_AIF_FMT_Msk & ((Value) << AUD_INT1_AIF_FMT_Pos))
#define AUD_INT1_AIF_FMT_RIGHT  WM8904_AIF_FMT(0)
#define AUD_INT1_AIF_FMT_LEFT   WM8904_AIF_FMT(1)
#define AUD_INT1_AIF_FMT_I2S    0x0002 /* WM8904_AIF_FMT(2) */
#define AUD_INT1_AIF_FMT_DSP    WM8904_AIF_FMT(3)

/*
 * R24 (0x18) - Audio Interface 0
 */
#define AUD_INT0_DACL_DATINV_Pos  12 /* DACL_DATINV */
#define AUD_INT0_DACL_DATINV      0x1000
#define AUD_INT0_DACR_DATINV_Pos  11 /* DACR_DATINV */
#define AUD_INT0_DACR_DATINV      0x0800
#define AUD_INT0_DAC_BOOST_Pos    9 /* DAC_BOOST - [10:9] */
#define AUD_INT0_DAC_BOOST_Msk    0x0600
#define AUD_INT0_DAC_BOOST(Value) (AUD_INT0_DAC_BOOST_Msk & ((Value) << AUD_INT0_DAC_BOOST_Pos))
#define AUD_INT0_LOOPBACK_Pos     8 /* LOOPBACK */
#define AUD_INT0_LOOPBACK         0x0100
#define AUD_INT0_AIFADCL_SRC_Pos  7 /* AIFADCL_SRC */
#define AUD_INT0_AIFADCL_SRC      0x0080
#define AUD_INT0_AIFADCR_SRC_Pos  6 /* AIFADCR_SRC */
#define AUD_INT0_AIFADCR_SRC      0x0040
#define AUD_INT0_AIFDACL_SRC_Pos  5 /* AIFDACL_SRC */
#define AUD_INT0_AIFDACL_SRC      0x0020
#define AUD_INT0_AIFDACR_SRC_Pos  4 /* AIFDACR_SRC */
#define AUD_INT0_AIFDACR_SRC      0x0010
#define AUD_INT0_ADC_COMP_Pos     3 /* ADC_COMP */
#define AUD_INT0_ADC_COMP         0x0008
#define AUD_INT0_ADC_COMPMODE_Pos 2 /* ADC_COMPMODE */
#define AUD_INT0_ADC_COMPMODE     0x0004
#define AUD_INT0_DAC_COMP_Pos     1 /* DAC_COMP */
#define AUD_INT0_DAC_COMP         0x0002
#define AUD_INT0_DAC_COMPMODE_Pos 0 /* DAC_COMPMODE */
#define AUD_INT0_DAC_COMPMODE     0x0001

/*
 * R116 (0x74) - FLL Control 1
 */
#define FLL_C1_FRACN_ENA (0x0004)
#define FLL_C1_OSC_ENA   (0x0002)
#define FLL_C1_FLL_ENA   (0x0001)

/*
 * R117 (0x75) - FLL Control 2
 */
#define FLL_C2_OUTDIV_Pos   8
#define FLL_C2_OUTDIV_Msk   0x3F00
/* Valid FLL_C2_OUTDIV() inputs are between 4 - 64 inclusive */
#define FLL_C2_OUTDIV(val)  ((((val) - 1) << FLL_C2_OUTDIV_Pos) & FLL_C2_OUTDIV_Msk)
#define FLL_C2_FRATIO_DIV1  (0b000)
#define FLL_C2_FRATIO_DIV2  (0b001)
#define FLL_C2_FRATIO_DIV4  (0b010)
#define FLL_C2_FRATIO_DIV8  (0b011)
#define FLL_C2_FRATIO_DIV16 (0b100)

/*
 * R118 (0x76) - FLL Control 3
 */
#define FLL_C3_K_Pos  0
#define FLL_C3_K_Msk  0xFFFF
#define FLL_C3_K(val) (((val) << FLL_C3_K_Pos) & FLL_C3_K_Msk)

/*
 * R119 (0x77) - FLL Control 4
 */
#define FLL_C4_N_Pos  5
#define FLL_C4_N_Msk  0x7FE0
#define FLL_C4_N(val) (((val) << FLL_C4_N_Pos) & FLL_C4_N_Msk)

/*
 * R120 (0x78) - FLL Control 5
 */
#define FLL_C5_CLK_REF_DIV1      (0b00 << 2)
#define FLL_C5_CLK_REF_DIV2      (0b01 << 2)
#define FLL_C5_CLK_REF_DIV4      (0b10 << 2)
#define FLL_C5_CLK_REF_DIV8      (0b11 << 2)
#define FLL_C5_CLK_REF_SRC_MCLK  (0b00)
#define FLL_C5_CLK_REF_SRC_BCLK  (0b01)
#define FLL_C5_CLK_REF_SRC_LRCLK (0b10)

/*
 * R127 (0x7F) - Interrupt Status
 */
#define WM8904_INT_STATUS_FLL_LOCK (1u << 2)

#endif /* _DRIVER_WM8904_H */
