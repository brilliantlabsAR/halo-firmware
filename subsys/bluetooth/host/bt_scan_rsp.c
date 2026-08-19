/* Copyright Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <errno.h>

#include "alif_ble.h"
#include "gapm.h"
#include "gap_le.h"
#include "gapm_le.h"
#include "gapm_le_adv.h"
#include "co_buf.h"
#include "alif/bluetooth/bt_scan_rsp.h"
#include "gap.h"

LOG_MODULE_REGISTER(bt_scan_rsp, CONFIG_BT_HOST_LOG_LEVEL);

#define BLE_MUTEX_TIMEOUT_MS 10000

/* Buffer for scan response data */
static co_buf_t *stored_scan_rsp_buf;

/* Semaphore for synchronizing buffer allocation */
K_SEM_DEFINE(scan_rsp_buf_sem, 0, 1);

/* Maximum scan response data length */
static uint16_t max_scan_rsp_data_len = 31; /* Default BLE spec value */

/**
 * @brief Initialize scan response data module
 *
 * @return 0 on success, negative error code otherwise
 */
int bt_scan_rsp_init(void)
{
	int err;

	/* Pre-allocate the buffer with maximum size */
	err = co_buf_alloc(&stored_scan_rsp_buf, 0, max_scan_rsp_data_len, 0);
	if (err) {
		LOG_ERR("Failed to pre-allocate scan response data buffer");
		return -ENOMEM;
	}

	/* Initialize buffer with zero data length and maximum tail length */
	stored_scan_rsp_buf->data_len = 0;
	stored_scan_rsp_buf->tail_len = max_scan_rsp_data_len;

	/* Give semaphore to indicate buffer is ready */
	k_sem_give(&scan_rsp_buf_sem);

	return 0;
}

int bt_scan_rsp_set(uint8_t actv_idx)
{
	co_buf_t *p_buf;
	int err;

	/* Allocate empty buffer for scan response data */
	err = co_buf_alloc(&p_buf, 0, 0, 0);
	if (err != 0) {
		LOG_ERR("Cannot allocate buffer for scan response data");
		return -ENOMEM;
	}

	/* Set scan response data */
	int lock_ret = alif_ble_mutex_lock(K_MSEC(BLE_MUTEX_TIMEOUT_MS));

	if (lock_ret) {
		__ASSERT(false, "BLE mutex lock timeout");
		co_buf_release(p_buf);
		return -ETIMEDOUT;
	}
	err = gapm_le_set_scan_response_data(actv_idx, p_buf);
	alif_ble_mutex_unlock();
	co_buf_release(p_buf);

	if (err) {
		LOG_ERR("Cannot set scan response data, error code: 0x%02x", err);
		return -EIO;
	}

	return 0;
}

/**
 * @brief Add data to scan response buffer
 *
 * This function adds a new AD structure with the specified type and data to the
 * scan response buffer. It handles the proper formatting of the AD structure
 * including length and type bytes.
 *
 * @param type AD type to add
 * @param data Pointer to the data to add
 * @param len Length of the data in bytes
 * @return 0 on success, negative error code otherwise
 */
static int add_scan_rsp_data(uint8_t type, const void *data, uint8_t len)
{
	uint8_t *buf_data;
	uint16_t current_len;
	uint8_t space_needed = len + 2; /* Length byte + type byte + data */

	/* Check if buffer exists */
	if (!stored_scan_rsp_buf) {
		LOG_ERR("Scan response buffer not allocated");
		return -EINVAL;
	}

	/* Check if we have enough space */
	if (space_needed > co_buf_tail_len(stored_scan_rsp_buf)) {
		return -ENOMEM;
	}

	/* Get current data length */
	current_len = co_buf_data_len(stored_scan_rsp_buf);

	/* Get pointer to data buffer */
	buf_data = co_buf_data(stored_scan_rsp_buf);

	/* Reserve space in the buffer for our data */
	uint8_t err = co_buf_tail_reserve(stored_scan_rsp_buf, space_needed);

	if (err != CO_BUF_ERR_NO_ERROR) {
		/* This should not happen since we checked space earlier */
		LOG_ERR("Failed to reserve space in scan response buffer");
		return -ENOMEM;
	}

	/* Get updated pointer after reservation */
	buf_data = co_buf_data(stored_scan_rsp_buf);

	/* Add length and type at the end of current data */
	buf_data[current_len] = len + 1;
	buf_data[current_len + 1] = type;

	/* Copy data after the type byte */
	memcpy(&buf_data[current_len + 2], data, len);

	return 0;
}

/**
 * @brief Update scan response data for an advertising activity
 *
 * This function creates a copy of the stored scan response buffer and
 * sends it to the controller to update the scan response data for the
 * specified advertising activity. The original buffer is kept intact
 * for future modifications.
 *
 * @param actv_idx Activity index of the advertising set
 * @return 0 on success, negative error code otherwise
 */
static int update_scan_rsp_data(uint8_t actv_idx)
{
	co_buf_t *scan_rsp_buf_final = NULL;
	int err;

	/* Check if buffer exists */
	if (!stored_scan_rsp_buf) {
		/* If for some reason the buffer doesn't exist, allocate a new one */
		err = co_buf_alloc(&stored_scan_rsp_buf, 0, max_scan_rsp_data_len, 0);
		if (err) {
			LOG_ERR("Failed to allocate buffer for scan response data");
			return -ENOMEM;
		}
		/* Initialize with zero data length and maximum tail length */
		stored_scan_rsp_buf->data_len = 0;
		stored_scan_rsp_buf->tail_len = max_scan_rsp_data_len;
	}

	/* Create a copy of the buffer so that we can keep modifying the original buffer */
	err = co_buf_alloc(&scan_rsp_buf_final, 0, 0, 0);
	if (err) {
		LOG_ERR("Failed to allocate buffer for final scan response data");
		return -ENOMEM;
	}

	/* Copy data from the stored buffer to the copy */
	uint16_t data_len = co_buf_data_len(stored_scan_rsp_buf);

	if (data_len > 0) {
		/* Reserve space in the copy buffer */
		err = co_buf_tail_reserve(scan_rsp_buf_final, data_len);
		if (err != CO_BUF_ERR_NO_ERROR) {
			LOG_ERR("Failed to reserve space in final scan response buffer");
			co_buf_release(scan_rsp_buf_final);
			return -ENOMEM;
		}

		/* Copy the data */
		memcpy(co_buf_data(scan_rsp_buf_final), co_buf_data(stored_scan_rsp_buf), data_len);
	}

	/* Set scan response data using the copy */
	int lock_ret = alif_ble_mutex_lock(K_MSEC(BLE_MUTEX_TIMEOUT_MS));

	if (lock_ret) {
		__ASSERT(false, "BLE mutex lock timeout");
		co_buf_release(scan_rsp_buf_final);
		return -ETIMEDOUT;
	}
	err = gapm_le_set_scan_response_data(actv_idx, scan_rsp_buf_final);
	alif_ble_mutex_unlock();
	co_buf_release(scan_rsp_buf_final);

	if (err) {
		LOG_ERR("Failed to set scan response data, error code: 0x%02x", err);
		return -EIO;
	}

	return 0;
}

int bt_scan_rsp_set_name(uint8_t actv_idx, const char *name, size_t name_len)
{
	int err;

	if (name == NULL || name_len == 0) {
		return -EINVAL;
	}

	/* Wait for buffer to be ready */
	if (k_sem_take(&scan_rsp_buf_sem, K_MSEC(100)) != 0) {
		LOG_ERR("Timeout waiting for scan response buffer");
		return -ETIMEDOUT;
	}

	/* Clear any existing data */
	if (stored_scan_rsp_buf) {
		uint16_t current_len = co_buf_data_len(stored_scan_rsp_buf);

		if (current_len > 0) {
			co_buf_tail_release(stored_scan_rsp_buf, current_len);
		}
	}

	/* Add name to scan response data */
	err = add_scan_rsp_data(GAP_AD_TYPE_COMPLETE_NAME, name, name_len);
	if (err) {
		LOG_ERR("Failed to add name to scan response data: %d", err);
		k_sem_give(&scan_rsp_buf_sem);
		return err;
	}

	/* Update scan response data */
	err = update_scan_rsp_data(actv_idx);

	/* Release semaphore */
	k_sem_give(&scan_rsp_buf_sem);

	return err;
}
