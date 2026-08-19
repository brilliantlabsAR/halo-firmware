/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef _DRIVERS_I2S_SYNC_H
#define _DRIVERS_I2S_SYNC_H

/**
 * @file
 * @brief I2S peripheral interface with synchronisation. The interface uses a callback-based
 * approach - when a block of data has been sent or received over the I2S bus, a user callback is
 * called. In the callback, the next block can be sent (TX direction) or buffer can be provided (RX
 * direction). The callback-based driver allows more precise control of the I2S timing than the
 * Zephyr API, since the user can check exactly when a block was completed in the callback.
 */

#include <zephyr/types.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>

enum i2s_sync_status {
	I2S_SYNC_STATUS_OK = 0,
	I2S_SYNC_STATUS_OVERRUN,
	I2S_SYNC_STATUS_RX_ERROR,
	I2S_SYNC_STATUS_TX_ERROR,
};

struct i2s_sync_config {
	uint32_t sample_rate;
	uint32_t bit_depth;
	uint8_t channel_count;
};

typedef void (*i2s_sync_cb_t)(const struct device *dev, enum i2s_sync_status status, void *buffer);

typedef int (*i2s_sync_api_register_cb_t)(const struct device *dev, enum i2s_dir dir,
					  i2s_sync_cb_t cb);
typedef int (*i2s_sync_api_send_t)(const struct device *dev, void *buf, size_t len);
typedef int (*i2s_sync_api_recv_t)(const struct device *dev, void *buf, size_t len);
typedef int (*i2s_sync_api_disable_t)(const struct device *dev, enum i2s_dir dir);
typedef int (*i2s_sync_api_get_config_t)(const struct device *dev, struct i2s_sync_config *cfg);
typedef int (*i2s_sync_api_set_config_t)(const struct device *dev, const struct i2s_sync_config *cfg);

__subsystem struct i2s_sync_driver_api {
	i2s_sync_api_register_cb_t register_cb;
	i2s_sync_api_send_t send;
	i2s_sync_api_recv_t recv;
	i2s_sync_api_disable_t disable;
	i2s_sync_api_get_config_t get_config;
	i2s_sync_api_set_config_t set_config;
};

/**
 * @brief Register a callback to be executed on completion of I2S transactions for the specified
 * device.
 *
 * @param dev Pointer to the device structure for the driver instance
 * @param dir Direction to register the callback for.
 * @param cb Callback to be called on I2S transaction completion
 *
 * @retval 0 if successful, negative error on failure
 */
__syscall int i2s_sync_register_cb(const struct device *dev, enum i2s_dir dir, i2s_sync_cb_t cb);

static inline int z_impl_i2s_sync_register_cb(const struct device *dev, enum i2s_dir dir,
					      i2s_sync_cb_t cb)
{
	const struct i2s_sync_driver_api *api = (const struct i2s_sync_driver_api *)dev->api;

	return api->register_cb(dev, dir, cb);
}

/**
 * @brief Send data over I2S
 *
 * @param dev Pointer to the device structure for the driver instance
 * @param buf Pointer to data to be transmitted
 * @param len Size in bytes of data to be transmitted
 *
 * @retval 0 if successful, negative error on failure
 */
__syscall int i2s_sync_send(const struct device *dev, void *buf, size_t len);

static inline int z_impl_i2s_sync_send(const struct device *dev, void *buf, size_t len)
{
	const struct i2s_sync_driver_api *api = (const struct i2s_sync_driver_api *)dev->api;

	return api->send(dev, buf, len);
}

/**
 * @brief Receive data over I2S
 *
 * @param dev Pointer to the device structure for the driver instance
 * @param buf Pointer to buffer for received data to be placed in
 * @param len Size of receive buffer in bytes
 *
 * @retval 0 if successful, negative error on failure
 */
__syscall int i2s_sync_recv(const struct device *dev, void *buf, size_t len);

static inline int z_impl_i2s_sync_recv(const struct device *dev, void *buf, size_t len)
{
	const struct i2s_sync_driver_api *api = (const struct i2s_sync_driver_api *)dev->api;

	return api->recv(dev, buf, len);
}

/**
 * @brief Disable I2S in given direction(s)
 *
 * @param dev Pointer to the device structure for the driver instance
 * @param dir Direction(s) to disable, may be I2S_DIR_TX, I2S_DIR_RX or I2S_DIR_BOTH
 *
 * @return 0 if successful, negative error on failure
 */
__syscall int i2s_sync_disable(const struct device *dev, enum i2s_dir dir);

static inline int z_impl_i2s_sync_disable(const struct device *dev, enum i2s_dir dir)
{
	const struct i2s_sync_driver_api *api = (const struct i2s_sync_driver_api *)dev->api;

	return api->disable(dev, dir);
}

/**
 * @brief Get I2S config parameters
 *
 * @param dev Pointer to the device structure for the driver instance
 * @param cfg Pointer to the i2s_sync_config structure to be filled with the I2S config parameters
 *
 * @return 0 if successful, negative error on failure
 */
__syscall int i2s_sync_get_config(const struct device *dev, struct i2s_sync_config *cfg);

static inline int z_impl_i2s_sync_get_config(const struct device *dev, struct i2s_sync_config *cfg)
{
	const struct i2s_sync_driver_api *api = (const struct i2s_sync_driver_api *)dev->api;

	return api->get_config(dev, cfg);
}

/**
 * @brief Dynamically reconfigure I2S parameters (must be idle in both directions).
 *
 * @param dev I2S sync device
 * @param cfg Desired configuration (sample_rate, bit_depth, channel_count)
 * @return 0 on success, -EBUSY if active, -EINVAL invalid params
 */
__syscall int i2s_sync_set_config(const struct device *dev, const struct i2s_sync_config *cfg);

static inline int z_impl_i2s_sync_set_config(const struct device *dev, const struct i2s_sync_config *cfg)
{
	const struct i2s_sync_driver_api *api = (const struct i2s_sync_driver_api *)dev->api;
	return api->set_config ? api->set_config(dev, cfg) : -ENOTSUP;
}

static inline int i2s_sync_configure(const struct device *dev, const struct i2s_sync_config *cfg)
{
	return i2s_sync_set_config(dev, cfg);
}

#include <syscalls/i2s_sync.h>

#endif /* _DRIVERS_I2S_SYNC_H */
