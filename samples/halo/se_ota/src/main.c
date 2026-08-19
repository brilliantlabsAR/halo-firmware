/*
 * Copyright (C) 2024 Alif Semiconductor.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/reboot.h>
#include <se_service.h>
#include "pupd.h"
#include "fupd.h"

#define TARGET_REVISION "SES A5 SE_FW_1.108.000-RC6 v1.108.0 Oct  8 2025 19:41:38"

#define FUPD 1
#define PUPD 2

#define UPDATE_MODE PUPD

#if UPDATE_MODE == FUPD
#define FIRMWARE_SIZE sizeof(_acfupd)
#define FIRMWARE_DATA _acfupd
#define UPDATE_TYPE_STR "FUPD"
#elif UPDATE_MODE == PUPD
#define FIRMWARE_SIZE sizeof(_acpupd)
#define FIRMWARE_DATA _acpupd
#define UPDATE_TYPE_STR "PUPD"
#endif

static uint8_t __attribute__((aligned(4))) aligned_firmware[FIRMWARE_SIZE];
#define DTCM_GLOBAL_BASE   DT_PROP(DT_NODELABEL(dtcm), dtcm_global_base)
#define DTCM_LOCAL_BASE    DT_REG_ADDR(DT_NODELABEL(dtcm))
#define LOCAL_TO_GLOBAL(x) (x - DTCM_LOCAL_BASE + DTCM_GLOBAL_BASE)
int main(void)
{
	uint8_t revision[80];
	int ret = se_service_get_se_revision(revision);
	if (ret) {
		printk("fetch_se_revision failed with %d\n", ret);
		return ret;
	}
	printk("Revision is %s\n", revision);


	if (strcmp((char *)revision, TARGET_REVISION) != 0) {
		memcpy(aligned_firmware, FIRMWARE_DATA, FIRMWARE_SIZE);
		uint32_t stoc_addr = (uint32_t)aligned_firmware;
		printk("Updating %s image in SE...%08X -> %08x of size %u\n",
		       UPDATE_TYPE_STR, stoc_addr, LOCAL_TO_GLOBAL(stoc_addr), (unsigned int)FIRMWARE_SIZE);
		ret = se_service_update_stoc(LOCAL_TO_GLOBAL(stoc_addr), FIRMWARE_SIZE);
		if (ret) {
			printk("se_service_update_stoc failed with %d\n", ret);
			return ret;
		}
		printk("SE %s update successful, wait rebooting...\n", UPDATE_TYPE_STR);
		for(int i=0;i<20;i++) {
			printk("*");
			k_sleep(K_MSEC(1000));
		}
		se_service_boot_reset_soc();
	} else {
		printk("SE firmware is already up to date.\n");
	}

	while (1) {
		printk(".");
		k_sleep(K_MSEC(1000));
	}

	return 0;
}
