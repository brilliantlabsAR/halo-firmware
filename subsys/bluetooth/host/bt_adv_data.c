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
#include "alif/bluetooth/bt_adv_data.h"

LOG_MODULE_REGISTER(bt_adv_data, CONFIG_BT_HOST_LOG_LEVEL);

#define GAPM_ADV_AD_TYPE_FLAGS_LENGTH 3
#define BLE_MUTEX_TIMEOUT_MS 10000

/**
 * @brief Advertising data buffer
 *
 */
static uint16_t max_adv_data_len = CONFIG_BLE_ADV_DATA_MAX; /* Will be updated from controller */
static co_buf_t *stored_adv_buf;

/* Semaphore for synchronizing buffer allocation */
K_SEM_DEFINE(adv_buf_sem, 0, 1);

/**
 * @brief Find an AD type in the advertising data
 *
 * @param type AD type to find
 * @param data_offset Pointer to store the offset to the data
 * @param data_len Pointer to store the length of the data
 * @return Offset to the AD type if found, -ENOENT otherwise
 */
static int find_ad_type(uint8_t type, uint8_t *data_offset, uint8_t *data_len)
{
	uint8_t offset = 0;
	uint8_t *data;
	uint16_t current_len;

	/* Check if buffer exists */
	if (!stored_adv_buf) {
		__ASSERT(0, "%s called with NULL buffer", __func__);
		return -ENOENT;
	}

	data = co_buf_data(stored_adv_buf);
	current_len = co_buf_data_len(stored_adv_buf);

	while (offset < current_len) {
		uint8_t field_len = data[offset];

		/* Check if we've reached the end of the data */
		if (field_len == 0) {
			break;
		}

		/* Check if this is the type we're looking for */
		if (data[offset + 1] == type) {
			*data_offset = offset + 2;
			*data_len = field_len - 1;
			return offset;
		}

		/* Move to the next AD field */
		offset += field_len + 1;
	}

	return -ENOENT;
}

/**
 * @brief Remove an AD type from the advertising data
 *
 * @param type AD type to remove
 * @return 0 on success, negative error code otherwise
 */
static int remove_ad_type(uint8_t type)
{
	uint8_t data_offset, data_len;
	int offset = find_ad_type(type, &data_offset, &data_len);
	uint8_t *data;
	uint16_t current_len;

	if (offset < 0) {
		return offset;
	}

	/* Buffer must exist since find_ad_type succeeded */
	__ASSERT(stored_adv_buf != NULL, "stored_adv_buf is NULL after successful find_ad_type");

	/* Get pointer to data buffer */
	data = co_buf_data(stored_adv_buf);

	/* Get current data length */
	current_len = co_buf_data_len(stored_adv_buf);

	/* Calculate total field length (length byte + type + data) */
	uint8_t field_len = data_len + 2;

	/* Remove the field by shifting the rest of the data */
	memmove(&data[offset], &data[offset + field_len], current_len - (offset + field_len));

	/* Release the space back to the tail */
	co_buf_tail_release(stored_adv_buf, field_len);

	LOG_DBG("Removed AD type 0x%02x, field length %u bytes", type, field_len);

	return 0;
}

/**
 * @brief Add an AD type to the advertising data
 *
 * @param type AD type to add
 * @param data Data to add
 * @param len Length of the data
 * @return 0 on success, negative error code otherwise
 */
static int add_ad_type(uint8_t type, const void *data, uint8_t len)
{
	uint8_t *buf_data;
	uint16_t current_len;
	uint8_t data_offset = 0, data_len = 0;

	/* Check if buffer exists */
	if (!stored_adv_buf) {
		__ASSERT(false, "stored_adv_buf is NULL");
		return -EINVAL;
	}

	/* Validate input parameters */
	__ASSERT(data != NULL || len == 0, "Data pointer is NULL but length > 0");
	__ASSERT(len <= 0xFF - 1, "Data length too large for AD structure");

	/* Check if there's an existing entry of this type */
	int find_result = find_ad_type(type, &data_offset, &data_len);

	/* Calculate how much space we need for the new data */
	uint8_t space_needed = len + 2; /* length byte + type + data */

	/* Check if there's enough space before making any changes */
	if (find_result >= 0) {
		/* We have existing data of this type */
		uint8_t existing_size = data_len + 2; /* length byte + type + data */

		/* Verify the existing data size makes sense */
		__ASSERT(data_len > 0, "Found AD type with zero data length");
		__ASSERT(existing_size <= co_buf_data_len(stored_adv_buf),
			 "Existing AD size exceeds buffer data length");

		/* Check if we have enough space: existing data size + tail space */
		if (space_needed > existing_size + co_buf_tail_len(stored_adv_buf)) {
			return -ENOMEM;
		}

		/* We have enough space, now remove the existing data */
		int remove_result = remove_ad_type(type);

		if (remove_result != 0) {
			__ASSERT(remove_result == 0, "Failed to remove existing AD type");
			return remove_result;
		}
	} else {
		/* No existing data, just check tail space */
		if (space_needed > co_buf_tail_len(stored_adv_buf)) {
			return -ENOMEM;
		}
	}

	/* Get current data length after potential removal */
	current_len = co_buf_data_len(stored_adv_buf);

	/* Get pointer to data buffer */
	buf_data = co_buf_data(stored_adv_buf);

	/* Reserve space in the buffer for our data */
	uint8_t err = co_buf_tail_reserve(stored_adv_buf, space_needed);

	if (err != CO_BUF_ERR_NO_ERROR) {
		/* This should not happen since we checked space earlier */
		__ASSERT(0, "Failed to reserve space even after space check");
		LOG_ERR("Failed to reserve space even after space check");
		return -ENOMEM;
	}

	/* Get updated pointer after reservation */
	buf_data = co_buf_data(stored_adv_buf);

	/* Add length and type at the end of current data */
	buf_data[current_len] = len + 1;
	buf_data[current_len + 1] = type;

	/* Add data */
	memcpy(&buf_data[current_len + 2], data, len);

	return 0;
}

/**
 * @brief Update advertising data for an activity
 *
 * @param actv_idx Activity index
 * @return 0 on success, negative error code otherwise
 */
static int update_adv_data(uint8_t actv_idx)
{
	int err;
	co_buf_t *adv_buf_final = NULL;

	/* Assert that buffer has been allocated */
	__ASSERT(stored_adv_buf != NULL, "Advertising data buffer not allocated");

	/* Create a copy of the buffer so that we can keep modifying the original buffer */
	err = co_buf_duplicate(stored_adv_buf, &adv_buf_final, 0, 0);
	if (err) {
		LOG_ERR("Failed to duplicate buffer for advertising data, error: %d", err);
		return -ENOMEM;
	}

	/* Get data length for logging */
	uint16_t data_len = co_buf_data_len(adv_buf_final);

	/* Log advertising data using hexdump */
	LOG_DBG("Advertising data (%u bytes):", data_len);
	LOG_HEXDUMP_DBG(co_buf_data(adv_buf_final), data_len, "ADV DATA");

	/* Set advertising data using the copy */
	int lock_ret = alif_ble_mutex_lock(K_MSEC(BLE_MUTEX_TIMEOUT_MS));

	if (lock_ret) {
		__ASSERT(false, "BLE mutex lock timeout");
		co_buf_release(adv_buf_final);
		return -ETIMEDOUT;
	}
	err = gapm_le_set_adv_data(actv_idx, adv_buf_final);
	alif_ble_mutex_unlock();

	/* If there was an error setting the advertising data */
	if (err) {
		__ASSERT(0, "Failed to set advertising data");
		/* Don't release the stored buffer since we want to keep it for future */
	}

	/* Always release the copy since it's no longer needed */
	co_buf_release(adv_buf_final);

	return err;
}

/**
 * @brief Get the name from advertising data (complete or shortened)
 *
 * @param name Buffer to store the device name
 * @param max_len Maximum length of the buffer
 * @return Length of the name on success, negative error code otherwise
 */
int bt_adv_data_get_name_auto(char *name, size_t max_len)
{
	if (name == NULL || max_len == 0) {
		return -EINVAL;
	}

	/* Try to get name from advertising data */
	uint8_t data_offset, data_len;
	int err = find_ad_type(GAP_AD_TYPE_COMPLETE_NAME, &data_offset, &data_len);

	/* If not found, try shortened name */
	if (err < 0) {
		err = find_ad_type(GAP_AD_TYPE_SHORTENED_NAME, &data_offset, &data_len);
	}

	/* If found in advertising data */
	if (err >= 0) {
		/* Get pointer to data buffer */
		uint8_t *data = co_buf_data(stored_adv_buf);

		/* Copy name to buffer, limited by max_len */
		size_t copy_len = MIN(data_len, max_len - 1);

		memcpy(name, &data[data_offset], copy_len);
		name[copy_len] = '\0';

		return data_len;
	}

	/* No name found in advertising data */
	name[0] = '\0';
	return -ENOENT;
}

/* Callback for max advertising data length query */
static void on_max_adv_data_len_cb(uint32_t metainfo, uint16_t status, uint16_t max_len)
{
	int err;

	/* TODO: Legacy advertising only for now */
	max_len = MIN(max_len, CONFIG_BLE_ADV_DATA_MAX);

	if (status == GAP_ERR_NO_ERROR) {
		LOG_INF("Controller supports maximum advertising data length of %u bytes", max_len);
		max_adv_data_len = max_len;
	} else {
		LOG_ERR("Failed to query maximum advertising data length, error: 0x%04x", status);
		/* Continue with default value */
	}

	/* Pre-allocate the buffer with maximum size from controller */
	err = co_buf_alloc(&stored_adv_buf, 0, max_adv_data_len, 0);
	if (err) {
		LOG_ERR("Failed to pre-allocate advertising data buffer");
	}

	/* Signal that buffer allocation is complete */
	k_sem_give(&adv_buf_sem);
}

/**
 * @brief Set default advertising data for an activity
 *
 * @param actv_idx Activity index
 * @param device_name Pointer to device name string
 * @param name_len Length of the device name
 * @return 0 on success, negative errno otherwise
 */
int bt_adv_data_set_default(uint8_t actv_idx, const char *device_name, size_t name_len)
{
	int err;

	if (device_name == NULL) {
		LOG_ERR("Device name pointer is NULL");
		return -EINVAL;
	}

	/* Clear any existing advertising data */
	if (stored_adv_buf) {
		/* Release all data back to tail */
		uint16_t current_len = co_buf_data_len(stored_adv_buf);

		if (current_len > 0) {
			co_buf_tail_release(stored_adv_buf, current_len);
		}
	}

	/* Note: Flags are not needed as they're handled by the RivieraWaves API */

	/* Add service UUID (placeholder "Dead Beef" UUID) */
	uint8_t uuid[16] = {0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD, 0xBE, 0xEF,
			    0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD, 0xBE, 0xEF};
	err = add_ad_type(GAP_AD_TYPE_COMPLETE_LIST_128_BIT_UUID, uuid, sizeof(uuid));
	if (err) {
		LOG_ERR("Failed to add service UUID to advertising data");
		return err;
	}

	/* Add device name with automatic shortening if needed */
	err = bt_adv_data_set_name_auto(actv_idx, device_name, name_len);
	if (err) {
		LOG_ERR("Failed to set device name in advertising data");
		return err;
	}

	return 0;
}

/**
 * @brief Initialize advertising data module
 *
 * @return 0 on success, negative error code otherwise
 */
int bt_adv_data_init(void)
{
	int ret;

	/* Release any stored buffer */
	co_buf_release(stored_adv_buf);
	stored_adv_buf = NULL;

	/* Reset semaphore in case it was previously given */
	k_sem_reset(&adv_buf_sem);

	/* Query the controller for maximum advertising data length
	 * Buffer will be allocated in the callback
	 */
	int lock_ret = alif_ble_mutex_lock(K_MSEC(BLE_MUTEX_TIMEOUT_MS));

	if (lock_ret) {
		__ASSERT(false, "BLE mutex lock timeout");
		return -ETIMEDOUT;
	}
	uint16_t gap_err = gapm_le_get_max_adv_data_len(0, on_max_adv_data_len_cb);

	alif_ble_mutex_unlock();

	if (gap_err != GAP_ERR_NO_ERROR) {
		LOG_WRN("Failed to query maximum advertising data length, error: 0x%04x", gap_err);
		/* Continue with default value, buffer will be allocated in the callback */
	}

	/* Wait for the callback to complete and allocate the buffer */
	ret = k_sem_take(&adv_buf_sem, K_SECONDS(5));
	if (ret != 0) {
		LOG_ERR("Timeout waiting for advertising data buffer allocation");
		return -ETIMEDOUT;
	}

	/* Check if buffer was successfully allocated  */
	if (!stored_adv_buf) {
		__ASSERT(false, "Failed to allocate advertising data buffer");
		return -ENOMEM;
	}

	/* Initialize buffer with zero data length and maximum tail length */
	stored_adv_buf->data_len = 0;
	stored_adv_buf->tail_len = max_adv_data_len;

	return 0;
}

/**
 * @brief Set manufacturer data in advertising data
 *
 * @param actv_idx Activity index for the advertising set
 * @param company_id Company identifier
 * @param data Manufacturer specific data
 * @param data_len Length of the manufacturer data
 * @return 0 on success, negative error code otherwise
 */
int bt_adv_data_set_manufacturer(uint8_t actv_idx, uint16_t company_id, const uint8_t *data,
				 size_t data_len)
{
	int err;

	if (data == NULL && data_len > 0) {
		LOG_ERR("Data pointer is NULL but data_len > 0");
		return -EINVAL;
	}

	/* add_ad_type will check for available space */
	if (!stored_adv_buf) {
		LOG_ERR("Advertising buffer not allocated");
		return -EINVAL;
	}

	/* TODO: Advertising data, legacy one, won't be more than 31 bytes in size and there will be
	 * other structures but let's consider this as the worst case scenario
	 */
	uint8_t manuf_data[CONFIG_BLE_ADV_DATA_MAX];

	/* Prepare manufacturer data with company ID in little-endian format */
	sys_put_le16(company_id, manuf_data);
	memcpy(manuf_data + 2, data, data_len);

	/* Add manufacturer data to advertising data */
	err = add_ad_type(GAP_AD_TYPE_MANU_SPECIFIC_DATA, manuf_data, data_len + 2);
	if (err) {
		return err;
	}

	/* Update advertising data */
	return update_adv_data(actv_idx);
}

/**
 * @brief Set service data in advertising data
 *
 * @param actv_idx Activity index for the advertising set
 * @param service_uuid Service UUID
 * @param data Service data
 * @param data_len Length of the service data
 * @return 0 on success, negative error code otherwise
 */
int bt_adv_data_set_service_data(uint8_t actv_idx, uint16_t service_uuid, const uint8_t *data,
				 size_t data_len)
{
	int err;

	if (data == NULL && data_len > 0) {
		LOG_ERR("Data pointer is NULL but data_len > 0");
		return -EINVAL;
	}

	/* add_ad_type will check for available space */
	if (!stored_adv_buf) {
		LOG_ERR("Advertising buffer not allocated");
		return -EINVAL;
	}

	/* TODO: Advertising data, legacy one, won't be more than 31 bytes in size and there will be
	 * other structures but let's consider this as the worst case scenario
	 */
	uint8_t service_data[CONFIG_BLE_ADV_DATA_MAX];

	/* Prepare service data with UUID in little-endian format */
	sys_put_le16(service_uuid, service_data);
	memcpy(service_data + 2, data, data_len);

	/* Add service data to advertising data */
	err = add_ad_type(GAP_AD_TYPE_SERVICE_16_BIT_DATA, service_data, data_len + 2);
	if (err) {
		return err;
	}

	/* Update advertising data */
	return update_adv_data(actv_idx);
}

/**
 * @brief Clear all advertising data
 *
 * @param actv_idx Activity index for the advertising set
 * @return 0 on success, negative error code otherwise
 */
int bt_adv_data_clear(uint8_t actv_idx)
{
	/* Reset advertising data */
	if (stored_adv_buf) {
		/* Release all data back to tail */
		uint16_t current_len = co_buf_data_len(stored_adv_buf);

		if (current_len > 0) {
			co_buf_tail_release(stored_adv_buf, current_len);
		}
	}

	/* Update advertising data (send empty data) */
	return update_adv_data(actv_idx);
}

/**
 * @brief Get current advertising data length
 *
 * @return Current advertising data length in bytes
 */
uint8_t bt_adv_data_get_length(void)
{
	if (!stored_adv_buf) {
		return 0;
	}
	return co_buf_data_len(stored_adv_buf);
}

/**
 * @brief Get pointer to raw advertising data
 *
 * @return Pointer to raw advertising data, or NULL if not available
 */
uint8_t *bt_adv_data_get_raw(void)
{
	if (!stored_adv_buf) {
		return NULL;
	}
	return co_buf_data(stored_adv_buf);
}

/**
 * @brief Check if advertising data contains a name
 *
 * @param name Buffer to store the name if found
 * @param max_len Maximum length of the name buffer
 * @return Length of the name on success, negative error code otherwise
 */
int bt_adv_data_check_name(char *name, size_t max_len)
{
	uint8_t data_offset, data_len;
	int err = find_ad_type(GAP_AD_TYPE_COMPLETE_NAME, &data_offset, &data_len);

	if (err < 0) {
		return err;
	}

	/* Get pointer to data buffer */
	uint8_t *data = co_buf_data(stored_adv_buf);

	/* Copy name to buffer, limited by max_len */
	size_t copy_len = MIN(data_len, max_len - 1);

	memcpy(name, &data[data_offset], copy_len);
	name[copy_len] = '\0';

	return data_len;
}

/**
 * @brief Set device name in advertising data, automatically using shortened name if needed
 *
 * This function automatically determines whether to use a complete or shortened name
 * based on the available space in the advertising data. If the complete name doesn't fit,
 * it will be truncated and set as a shortened name.
 *
 * @param actv_idx Activity index for the advertising set
 * @param name Device name to set
 * @param name_len Length of the device name
 * @return 0 on success, negative error code otherwise
 */
int bt_adv_data_set_name_auto(uint8_t actv_idx, const char *name, size_t name_len)
{
	int err;

	/* Parameter validation */

	if (!name) {
		LOG_ERR("Name pointer is NULL");
		return -EINVAL;
	}

	if (name_len == 0) {
		LOG_ERR("Name length is zero");
		return -EINVAL;
	}

	if (!stored_adv_buf) {
		LOG_ERR("Advertising buffer not allocated");
		return -EINVAL;
	}

	/* Calculate available space for the name using buffer's tail length
	 * Each AD structure has 2 bytes overhead (length + type)
	 * Unfortunately we need to account for the flags field as well, even though those are set
	 * by the controller implicitly
	 */
	uint16_t available_space = co_buf_tail_len(stored_adv_buf) - GAPM_ADV_AD_TYPE_FLAGS_LENGTH;

	/* Check if we have existing name fields that we need to account for */
	uint8_t data_offset_complete = 0, data_len_complete = 0;
	uint8_t data_offset_short = 0, data_len_short = 0;
	bool has_complete_name = (find_ad_type(GAP_AD_TYPE_COMPLETE_NAME, &data_offset_complete,
					       &data_len_complete) >= 0);
	bool has_short_name = (find_ad_type(GAP_AD_TYPE_SHORTENED_NAME, &data_offset_short,
					    &data_len_short) >= 0);

	/* If we have existing names, we'll remove them and reclaim their space */
	if (has_complete_name) {
		available_space +=
			data_len_complete + 2; /* Add back the space used by complete name */
	}

	if (has_short_name) {
		available_space +=
			data_len_short + 2; /* Add back the space used by shortened name */
	}

	/* Now determine if we need to use shortened name based on available space */
	uint8_t ad_type;
	size_t final_name_len;

	/* Account for length and type bytes in our new name */
	uint8_t name_overhead = 2; /* length + type */

	if (name_len + name_overhead <= available_space) {
		/* Complete name fits */
		ad_type = GAP_AD_TYPE_COMPLETE_NAME;
		final_name_len = name_len;
		LOG_DBG("Using complete name, length: %zu", final_name_len);
	} else if (available_space > name_overhead) {
		/* Need to use shortened name */
		ad_type = GAP_AD_TYPE_SHORTENED_NAME;
		final_name_len = available_space - name_overhead;
		LOG_DBG("Using shortened name, length: %zu (original: %zu)", final_name_len,
			name_len);
	} else {
		__ASSERT(0, "No space available for name in advertising data");
		LOG_ERR("No space available for name in advertising data");
		return -ENOMEM;
	}

	/* Now remove any existing names before adding the new one */
	if (has_complete_name) {
		/* Remove complete name */
		remove_ad_type(GAP_AD_TYPE_COMPLETE_NAME);
		LOG_DBG("Removed existing complete name");
	}

	if (has_short_name) {
		/* Remove shortened name */
		remove_ad_type(GAP_AD_TYPE_SHORTENED_NAME);
		LOG_DBG("Removed existing shortened name");
	}

	/* Add name to advertising data */
	err = add_ad_type(ad_type, name, final_name_len);
	if (err) {
		return err;
	}

	/* Update advertising data */
	return update_adv_data(actv_idx);
}
