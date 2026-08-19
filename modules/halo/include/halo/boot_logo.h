/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * Boot logo splash: draws an LZ4-compressed 4-bit indexed bitmap straight to
 * the display framebuffer during early boot, before the Lua runtime (and
 * main.lua) start. The display boots in power-save mode, so show() resumes the
 * panel first and hide() returns it to power-save afterwards, leaving the
 * documented "apps must re-enable the display" contract intact.
 */

#ifndef HALO_BOOT_LOGO_H_
#define HALO_BOOT_LOGO_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring the display out of power-save and draw the boot logo.
 *
 * Performs (idempotent) display hardware init, resumes the panel, then
 * decompresses and draws the centred logo. Safe to call before the Lua
 * runtime is initialised.
 *
 * @param label Optional text drawn centred under the logo (e.g. the BLE
 *              device name when the device is unpaired), or NULL for none.
 * @return 0 on success, negative errno on failure.
 */
int halo_display_boot_logo_show(const char *label);

/**
 * @brief Return the display to power-save mode after the logo.
 *
 * @return 0 on success, negative errno on failure.
 */
int halo_display_boot_logo_hide(void);

/* Generated logo asset (see modules/halo/tools/gen_boot_logo.py). */
extern const uint16_t halo_boot_logo_width;
extern const uint16_t halo_boot_logo_height;
extern const uint8_t halo_boot_logo_palette[16][3];
extern const uint32_t halo_boot_logo_raw_size;
extern const uint8_t halo_boot_logo_lz4[];
extern const uint32_t halo_boot_logo_lz4_size;

#ifdef __cplusplus
}
#endif

#endif /* HALO_BOOT_LOGO_H_ */
