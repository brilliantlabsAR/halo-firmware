/* Copyright (c) 2025 Alif Semiconductor
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SE_MGMT_H
#define SE_MGMT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * SE management group ID
 */
#define MGMT_GROUP_ID_SE 64

/**
 * SE management command IDs
 */
enum se_mgmt_command_id {
	/** Update SE firmware */
	SE_MGMT_ID_UPDATE = 0,
	/** Get SE version */
	SE_MGMT_ID_VERSION = 1,
};

/**
 * SE management error codes
 */
enum se_mgmt_error_code {
	/** No error */
	SE_MGMT_ERR_OK = 0,
	/** Invalid firmware */
	SE_MGMT_ERR_INVALID_FIRMWARE = 1,
	/** Update failed */
	SE_MGMT_ERR_UPDATE_FAILED = 2,
	/** Version mismatch */
	SE_MGMT_ERR_VERSION_MISMATCH = 3,
	/** Insufficient memory */
	SE_MGMT_ERR_NO_MEMORY = 4,
};

#ifdef __cplusplus
}
#endif

#endif /* SE_MGMT_H */