/**
 * @file sm.h
 * @brief Ship Mode Driver API
 *
 * Copyright (C) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_SM_H_
#define ZEPHYR_INCLUDE_DRIVERS_SM_H_

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Shutdown device and enter ship mode
 *
 * Triggers ship mode by setting the control GPIO high, putting the
 * device into ultra-low power state. Device can only be woken up
 * by hardware reset or power cycle.
 *
 * @param dev Pointer to the ship mode device
 * @return 0 on success, negative errno on failure
 */
int shutdown(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_SM_H_ */