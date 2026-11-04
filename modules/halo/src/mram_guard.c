/* Copyright (c) 2026 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * MPU write guard over the parts of MRAM that the M55 must never write.
 *
 * MRAM is written by plain CPU stores (flash_mram_alif.c issues two 64-bit
 * stores per 16-byte row, with no unlock step), and the SoC's fixed MPU region
 * FLASH_0 maps all 2 MB at 0x80000000 read-write because the flash driver
 * needs CONFIG_MPU_ALLOW_FLASH_WRITE. A stray store anywhere in MRAM therefore
 * commits, including to the boot image (0x80000000, the bootloader) and to the
 * SE's area at the top (ATOC, DEVICE object, SES).
 *
 * This splits FLASH_0 so only the partitions that are legitimately written
 * (slot0, slot1, scratch, storage) stay writable, and maps the boot partition
 * and the SE-reserved area read-only. The SE writes MRAM through its own bus
 * master, so station/ISP writes of the bootloader are unaffected; nothing on
 * the M55 writes the boot partition (OTA only writes slot1, MCUboot only swaps
 * slot0/slot1/scratch).
 *
 * The boot region stays executable (MCUboot runs from it); the SE area is XN.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/devicetree.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>
#include <cmsis_core.h>

#include "halo/mram_guard.h"

LOG_MODULE_REGISTER(mram_guard, CONFIG_HALO_LOG_LEVEL);

#define MRAM_NODE  DT_NODELABEL(mram_storage)
#define MRAM_BASE  ((uint32_t)DT_REG_ADDR(MRAM_NODE))
#define MRAM_END   (MRAM_BASE + (uint32_t)DT_REG_SIZE(MRAM_NODE))
#define BOOT_START (MRAM_BASE + (uint32_t)FIXED_PARTITION_OFFSET(boot_partition))
#define BOOT_END   (BOOT_START + (uint32_t)FIXED_PARTITION_SIZE(boot_partition))
#define SE_START   (MRAM_BASE + (uint32_t)FIXED_PARTITION_OFFSET(reserved_for_se))

BUILD_ASSERT(BOOT_START == MRAM_BASE, "boot partition must start MRAM");
BUILD_ASSERT((BOOT_END % 32) == 0 && (SE_START % 32) == 0, "MPU regions are 32-byte granular");
BUILD_ASSERT(SE_START > BOOT_END, "SE area must sit above the boot partition");

#define RBAR_BASE(a)  ((a) & MPU_RBAR_BASE_Msk)
#define RLAR_LIMIT(a) (((a) - 1U) & MPU_RLAR_LIMIT_Msk)
#define AP_RO_ANY     (3U << MPU_RBAR_AP_Pos)

static int guard_status = -ENODEV;

static uint32_t mpu_num_regions(void)
{
	return (MPU->TYPE & MPU_TYPE_DREGION_Msk) >> MPU_TYPE_DREGION_Pos;
}

static int mram_guard_init(void)
{
	uint32_t n = mpu_num_regions();
	int flash_idx = -1;
	int free_idx[2] = {-1, -1};
	int nfree = 0;

	/* Find the SoC's FLASH_0 region (exactly the whole MRAM). */
	for (uint32_t i = 0; i < n; i++) {
		MPU->RNR = i;
		uint32_t rbar = MPU->RBAR;
		uint32_t rlar = MPU->RLAR;

		if ((rlar & MPU_RLAR_EN_Msk) &&
		    (rbar & MPU_RBAR_BASE_Msk) == RBAR_BASE(MRAM_BASE) &&
		    (rlar & MPU_RLAR_LIMIT_Msk) == RLAR_LIMIT(MRAM_END)) {
			flash_idx = i;
		}
	}
	/* Take the highest free slots, away from where Zephyr allocates. */
	for (int i = (int)n - 1; i >= 0 && nfree < 2; i--) {
		MPU->RNR = i;
		if (!(MPU->RLAR & MPU_RLAR_EN_Msk)) {
			free_idx[nfree++] = i;
		}
	}
	if (flash_idx < 0 || nfree < 2) {
		guard_status = (flash_idx < 0) ? -ENOENT : -ENOSPC;
		return 0;
	}

	unsigned int key = irq_lock();
	uint32_t ctrl = MPU->CTRL;

	MPU->CTRL = 0;
	__DSB();
	__ISB();

	/* FLASH_0 shrinks to [end of boot partition, start of SE area), same attrs. */
	MPU->RNR = flash_idx;
	uint32_t rbar = MPU->RBAR;
	uint32_t rlar = MPU->RLAR;

	MPU->RBAR = (rbar & ~MPU_RBAR_BASE_Msk) | RBAR_BASE(BOOT_END);
	MPU->RLAR = (rlar & ~MPU_RLAR_LIMIT_Msk) | RLAR_LIMIT(SE_START);

	/* Boot partition: read-only, executable, same memory attributes. */
	MPU->RNR = free_idx[0];
	MPU->RBAR = RBAR_BASE(BOOT_START) | (rbar & MPU_RBAR_SH_Msk) | AP_RO_ANY;
	MPU->RLAR = RLAR_LIMIT(BOOT_END) | (rlar & MPU_RLAR_AttrIndx_Msk) | MPU_RLAR_EN_Msk;

	/* SE area (ATOC, DEVICE, SES): read-only, execute-never. */
	MPU->RNR = free_idx[1];
	MPU->RBAR = RBAR_BASE(SE_START) | (rbar & MPU_RBAR_SH_Msk) | AP_RO_ANY | MPU_RBAR_XN_Msk;
	MPU->RLAR = RLAR_LIMIT(MRAM_END) | (rlar & MPU_RLAR_AttrIndx_Msk) | MPU_RLAR_EN_Msk;

	MPU->CTRL = ctrl;
	__DSB();
	__ISB();
	irq_unlock(key);

	guard_status = 0;
	return 0;
}

/* Right after the arch MPU init (which runs before PRE_KERNEL_1). */
SYS_INIT(mram_guard_init, PRE_KERNEL_1, 0);

int halo_mram_guard_status(void)
{
	return guard_status;
}

uint32_t halo_mram_tt(uint32_t addr)
{
	uint32_t r;

	__asm__ volatile("tt %0, %1" : "=r"(r) : "r"(addr));
	return r;
}

bool halo_mram_writable(uint32_t addr)
{
	uint32_t r = halo_mram_tt(addr);

	/* MRVALID clear: no region matched (background map, MRAM is then
	 * writable when privileged); RW bit (19) set: writable. */
	return !(r & BIT(16)) || (r & BIT(19));
}

static int mram_guard_report(void)
{
	if (guard_status) {
		LOG_ERR("MRAM guard NOT active (%d): boot image and ATOC are CPU-writable",
			guard_status);
	} else {
		LOG_INF("MRAM guard: boot 0x%08x-0x%08x %s, SE 0x%08x-0x%08x %s", BOOT_START,
			BOOT_END - 1, halo_mram_writable(BOOT_START) ? "RW!" : "RO", SE_START,
			MRAM_END - 1, halo_mram_writable(SE_START) ? "RW!" : "RO");
	}
	return 0;
}
SYS_INIT(mram_guard_report, APPLICATION, 99);
