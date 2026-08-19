/* Copyright (c) 2025 PixArt Imaging Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once
#include <zephyr/kernel.h>
#include <zephyr/device.h>

#include <zephyr/drivers/video.h>

#define PAG7982_ID_L 0x82
#define PAG7982_ID_H 0x79

#define BANK_SEL 0xEF
#define UPDATE 0xEB

// BANK0
#define PART_ID_L          0x00
#define PART_ID_H          0x01
#define INTFLAG_1          0x04
#define R_GPIO0_SEL        0x06
#define R_GPIO1_SEL        0x07
#define R_GPIO2_SEL        0x08
#define T_GPIO0            0x0E
#define T_GPIO1            0x0F
#define T_GPIO2            0x10
#define T_ROW              0x1C
#define R_TRG_FRAME        0x2E
#define R_TRG_EN           0x30
#define R_TRG_MODE         0x31
#define T_COL              0x3E
#define T_HSYNC            0x42
#define T_VSYNC            0x43
#define R_FRAMETIME_0      0x4C
#define R_FRAMETIME_1      0x4D
#define R_FRAMETIME_2      0x4E
#define R_FRAMETIME_3      0x4F
#define R_WOI_EN           0x80
#define R_WOI_ZONE         0x81
#define R_WOI_HSTART1      0x82
#define R_WOI_VSTART1      0x83
#define R_WOI_HEND1        0x84
#define R_WOI_VEND1        0x85
#define R_WOI_HSTART2      0x86
#define R_WOI_VSTART2      0x87
#define R_WOI_HEND2        0x88
#define R_WOI_VEND2        0x89
#define R_WOI_HSTART3      0x8A
#define R_WOI_VSTART3      0x8B
#define R_WOI_HEND3        0x8C
#define R_WOI_VEND3        0x8D
#define R_WOI_HSTART4      0x8E
#define R_WOI_VSTART4      0x8F
#define R_WOI_HEND4        0x90
#define R_WOI_VEND4        0x91
#define MOTION_MODE        0x93
#define MOTION             0x94
#define WOI1_DIFF_CNT_L    0x97
#define WOI1_DIFF_CNT_H    0x98
#define WOI2_DIFF_CNT_L    0x99
#define WOI2_DIFF_CNT_H    0x9A
#define WOI3_DIFF_CNT_L    0x9B
#define WOI3_DIFF_CNT_H    0x9C
#define WOI4_DIFF_CNT_L    0x9D
#define WOI4_DIFF_CNT_H    0x9E
#define FRAME_DIFF_XMIN    0x9F
#define FRAME_DIFF_XMAX    0xA0
#define FRAME_DIFF_YMIN    0xA1
#define FRAME_DIFF_YMAX    0xA2
#define PARALLEL_INV       0xAF
#define R_WOI1_FRAME_THD_L 0xD0
#define R_WOI1_FRAME_THD_H 0xD1
#define R_WOI2_FRAME_THD_L 0xD3
#define R_WOI2_FRAME_THD_H 0xD4
#define R_WOI3_FRAME_THD_L 0xD5
#define R_WOI3_FRAME_THD_H 0xD7
#define R_WOI4_FRAME_THD_L 0xD9
#define R_WOI4_FRAME_THD_H 0xDA
#define R_EXTEND_VSYNC_EN  0xE6

// BANK1
#define R_ANALOG_SKIP_CTL   0x4B
#define CMD_AND_ROISKIP_CTL 0x70
#define R_V_SETTLE_START_L  0xC2
#define R_V_SETTLE_START_H  0xC3
#define R_V_START_L         0xCB
#define R_V_START_H         0xCC
#define R_FLIP              0xCE

// BANK2
#define R_EXPO_LED_EARLY    0x7A
#define R_EXPO_LED_POLARITY 0x7C
#define R_AB_SWITCH         0xD9

// BANK4
#define R_ISP_EN            0x00
#define R_ISP_TEST_MODE     0x0A
#define R_AE_EXPO_MANUAL    0x30
#define R_YTAR              0x31
#define R_AE_LOCK_RANGE     0x32
#define R_AE_LOCK_OUT_L     0x33
#define R_AE_LOCK_OUT_H     0x34
#define R_AE_START_DIV4_X   0x38
#define R_AE_START_DIV4_Y   0x39
#define R_AE_SIZE_DIV4_X    0x3A
#define R_AE_SIZE_DIV4_Y    0x3B
#define R_AE_MIN_GAIN_H     0x41
#define R_AE_MAX_XGAIN_L    0x42
#define R_AE_MAX_XGAIN_H    0x43
#define R_AE_MIN_EXPO_1     0x44
#define R_AE_MIN_EXPO_2     0x45
#define R_AE_MIN_EXPO_3     0x46
#define R_AE_MIN_EXPO_4     0x47
#define R_AE_MAX_EXPO_1     0x48
#define R_AE_MAX_EXPO_2     0x49
#define R_AE_MAX_EXPO_3     0x4A
#define R_AE_MAX_EXPO_4     0x4B
#define R_AE_GAIN_MANUAL_L  0x51
#define R_AE_GAIN_MANUAL_H  0x52
#define R_AE_EXPO_MANUAL_1  0x53
#define R_AE_EXPO_MANUAL_2  0x54
#define R_AE_EXPO_MANUAL_3  0x55
#define R_AE_EXPO_MANUAL_4  0x56
#define R_COLOR_GAIN_R_L    0x6E
#define R_COLOR_GAIN_R_H    0x6F
#define R_COLOR_GAIN_G_L    0x70
#define R_COLOR_GAIN_G_H    0x71
#define R_COLOR_GAIN_B_L    0x72
#define R_COLOR_GAIN_B_H    0x73
#define R_ISP_WOI_EN        0x7B
#define R_ISP_WOI_HSIZE_L   0x7C
#define R_ISP_WOI_HSIZE_H   0x7D
#define R_ISP_WOI_VSIZE_L   0x7E
#define R_ISP_WOI_VSIZE_H   0x7F
#define R_ISP_WOI_HOFFSET_L 0x80
#define R_ISP_WOI_HOFFSET_H 0x81
#define R_ISP_WOI_VOFFSET_L 0x82
#define R_ISP_WOI_VOFFSET_H 0x83

#define PAG7982_PM_TURN_OFF (VIDEO_CTRL_CLASS_CAMERA + 100)
#define PAG7982_PM_TURN_ON  (VIDEO_CTRL_CLASS_CAMERA + 101)
