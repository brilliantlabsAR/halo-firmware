/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_DISPLAY_VGA020_H_
#define ZEPHYR_INCLUDE_DRIVERS_DISPLAY_VGA020_H_

#include <zephyr/device.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set display pan/shift offset
 *
 * @param dev Pointer to the display device
 * @param x_offset Horizontal pan offset (-50 to +50)
 * @param y_offset Vertical pan offset (-50 to +50)
 *
 * @return 0 on success, negative errno on failure
 */
int vga020_set_pan(const struct device *dev, int8_t x_offset, int8_t y_offset);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_DISPLAY_VGA020_H_ */
