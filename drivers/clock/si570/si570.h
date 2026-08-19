/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef _DRIVER_SI570_H
#define _DRIVER_SI570_H

#include <zephyr/sys/util_macro.h>

#define SI570_REG_HIGH_SPEED_N1_DIVS       0x7
#define SI570_REG_N1_REFERENCE_FREQUENCY_0 0x8
#define SI570_REG_REFERENCE_FREQUENCY_1    0x9
#define SI570_REG_REFERENCE_FREQUENCY_2    0xA
#define SI570_REG_REFERENCE_FREQUENCY_3    0xB
#define SI570_REG_REFERENCE_FREQUENCY_4    0xC
#define SI570_REG_RESET_FREEZE_CONTROL     0x87
#define SI570_REG_FREEZE_DCO               0x89

#define SI570_BIT_RST_REG      BIT(7)
#define SI570_BIT_NEWFREQ      BIT(6)
#define SI570_BIT_FREEZE_M     BIT(5)
#define SI570_BIT_FREEXE_VCADC BIT(4)
#define SI570_BIT_RECALL       BIT(0)

#define SI570_BIT_FREEZE_DCO BIT(4)

#define SI570_RSHIFT_HS_DIV 5
#define SI570_MASK_HS_DIV   (~BIT_MASK(5))
#define SI570_OFFSET_HS_DIV 4

#define SI570_LSHIFT_N1_6_2 2
#define SI570_MASK_N1_6_2   BIT_MASK(5)
#define SI570_RSHIFT_N1_1_0 6
#define SI570_MASK_N1_1_0   (~BIT_MASK(6))
#define SI570_OFFSET_N1     1

#define SI570_MASK_RFREQ_37_32   BIT_MASK(6)
#define SI570_LSHIFT_RFREQ_37_32 32
#define SI570_LSHIFT_RFREQ_31_24 24
#define SI570_LSHIFT_RFREQ_23_16 16
#define SI570_LSHIFT_RFREQ_15_8  8

#define SI570_FDCO_MIN 4850000000ULL
#define SI570_FDCO_MID 5260000000ULL
#define SI570_FDCO_MAX 5670000000ULL

#define SI570_FREQUENCY_MIN 10000000LL
#define SI570_FREQUENCY_MAX 945000000LL

#define SI570_MAX_CHANGE_WITHOUT_INTERRUPTION_PPM 3500

/* No required delay between I2C transactions is documented in the SI570 data sheet. However it has
 * been observed that performing back to back I2C reads and writes with no gap in between causes
 * inconsistent results (in particular when updating the frequency, having no delay between
 * transactions causes the chip to get stuck in a state where the output clock keeps stopping for
 * ~1ms every few ms). The minimum delay to avoid this effect has been determined through trial and
 * error.
 */
#define SI570_TRANSACTION_GAP_US 40

#define SI570_LARGE_FREQUENCY_SETTLING_TIME_USEC 10000
#define SI570_SMALL_FREQUENCY_SETTLING_TIME_USEC 100
#define SI570_POWER_UP_TIME_USEC                 10000

#endif /* _DRIVER_SI570_H */
