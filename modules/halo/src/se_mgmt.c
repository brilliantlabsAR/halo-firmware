/* Copyright (c) 2025 Alif Semiconductor
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/sys/reboot.h>
#include <se_service.h>
#include <string.h>
#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>

#include "halo/se_mgmt.h"
#include "halo/mem_manager.h"
#include "halo/ble_connection.h"

LOG_MODULE_REGISTER(se_mgmt, CONFIG_HALO_LOG_LEVEL);

/* SE firmware buffer - dynamically allocated from internal SRAM */
static uint8_t *se_firmware_buffer = NULL;
static size_t se_firmware_size = 0;

static struct k_work_delayable reboot_work;

static void reboot_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	LOG_WRN("Rebooting to apply SE update...");
	halo_ble_conn_prepare_reboot();
	sys_reboot(SYS_REBOOT_COLD);
}

/**
 * SE firmware update handler
 */
static int se_mgmt_update(struct smp_streamer *ctxt)
{
	struct zcbor_string firmware_data = { 0 };
	uint32_t offset = 0;
	uint32_t total_size = 0;
	zcbor_state_t *zse = ctxt->writer->zs;
	zcbor_state_t *zsd = ctxt->reader->zs;
	bool ok;
	size_t decoded;
	static bool transfer_in_progress = false;
	static uint32_t expected_total_size = 0;

	struct zcbor_map_decode_key_val se_update_decode[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("image", zcbor_bstr_decode, &firmware_data),
		ZCBOR_MAP_DECODE_KEY_DECODER("offset", zcbor_uint32_decode, &offset),
		ZCBOR_MAP_DECODE_KEY_DECODER("len", zcbor_uint32_decode, &total_size),
	};

	ok = zcbor_map_decode_bulk(zsd, se_update_decode, ARRAY_SIZE(se_update_decode),
				   &decoded) == 0;

	if (!ok || firmware_data.len == 0) {
		LOG_ERR("Invalid SE update parameters");
		return MGMT_ERR_EINVAL;
	}

	/* First packet - initialize transfer */
	if (offset == 0) {
		if (total_size == 0) {
			LOG_ERR("Invalid total size: %u", total_size);
			return MGMT_ERR_EINVAL;
		}
		
		/* Allocate buffer from internal SRAM based on actual firmware size */
		if (se_firmware_buffer == NULL) {
			se_firmware_buffer = halo_malloc(total_size, HALO_MEM_REGION_INTERNAL);
			if (se_firmware_buffer == NULL) {
				LOG_ERR("Failed to allocate %u bytes from internal SRAM for SE firmware buffer",
					total_size);
				return MGMT_ERR_ENOMEM;
			}
			LOG_DBG("Allocated %u bytes from internal SRAM for SE firmware buffer", total_size);
		}
		
		expected_total_size = total_size;
		se_firmware_size = 0;
		transfer_in_progress = true;
		LOG_INF("Starting SE firmware transfer, total size: %u", total_size);
	} else if (!transfer_in_progress) {
		LOG_ERR("Transfer not initialized");
		return MGMT_ERR_EINVAL;
	}

	/* Check if buffer is allocated */
	if (se_firmware_buffer == NULL) {
		LOG_ERR("SE firmware buffer not allocated");
		transfer_in_progress = false;
		return MGMT_ERR_EUNKNOWN;
	}

	/* Validate offset and data size */
	if (offset + firmware_data.len > expected_total_size) {
		LOG_ERR("Data exceeds expected size: offset=%u, len=%u, expected=%u",
			offset, firmware_data.len, expected_total_size);
		transfer_in_progress = false;
		halo_free(se_firmware_buffer);
		se_firmware_buffer = NULL;
		return MGMT_ERR_EINVAL;
	}

	/* Copy data chunk to buffer */
	memcpy(se_firmware_buffer + offset, firmware_data.value, firmware_data.len);
	se_firmware_size = offset + firmware_data.len;

	/* Check if transfer is complete */
	if (se_firmware_size >= expected_total_size) {
		transfer_in_progress = false;
		LOG_DBG("SE firmware transfer complete, size=%u", se_firmware_size);

		/* Apply the update */
		int ret = se_service_update_stoc(se_firmware_buffer, se_firmware_size);
		
		/* Free buffer after update attempt */
		halo_free(se_firmware_buffer);
		se_firmware_buffer = NULL;
		
		if (ret != 0) {
			LOG_ERR("SE update failed: %d", ret);
			ok = smp_add_cmd_err(zse, MGMT_GROUP_ID_SE, SE_MGMT_ERR_UPDATE_FAILED);
			goto end;
		}

		LOG_WRN("SE firmware update successful, rebooting...");

		/* Response before reboot */
		ok = zcbor_tstr_put_lit(zse, "status") &&
		     zcbor_tstr_put_lit(zse, "success") &&
		     zcbor_tstr_put_lit(zse, "message") &&
		     zcbor_tstr_put_lit(zse, "Device will reboot to apply SE update");

		k_work_init_delayable(&reboot_work, reboot_work_handler);
		k_work_schedule(&reboot_work, K_MSEC(100));
	} else {
		/* Transfer not complete, send progress response */
		ok = zcbor_tstr_put_lit(zse, "status") &&
		     zcbor_tstr_put_lit(zse, "progress") &&
		     zcbor_tstr_put_lit(zse, "received") &&
		     zcbor_uint32_put(zse, se_firmware_size) &&
		     zcbor_tstr_put_lit(zse, "total") &&
		     zcbor_uint32_put(zse, expected_total_size);
	}

end:
	return (ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE);
}

/**
 * Get SE version handler
 */
static int se_mgmt_version(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	bool ok;
	uint8_t version[128];

	memset(version, 0, sizeof(version));

	int ret = se_service_get_se_revision(version);
	if (ret != 0) {
		LOG_ERR("Failed to get SE version: %d", ret);
		return MGMT_ERR_EUNKNOWN;
	}

	ok = zcbor_tstr_put_lit(zse, "version") &&
	     zcbor_tstr_put_term(zse, (char *)version, strlen((char *)version));

	return (ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE);
}

static const struct mgmt_handler se_mgmt_handlers[] = {
	[SE_MGMT_ID_UPDATE] = {
		.mh_read = NULL,
		.mh_write = se_mgmt_update,
	},
	[SE_MGMT_ID_VERSION] = {
		.mh_read = se_mgmt_version,
		.mh_write = NULL,
	},
};

static struct mgmt_group se_mgmt_group = {
	.mg_handlers = se_mgmt_handlers,
	.mg_handlers_count = ARRAY_SIZE(se_mgmt_handlers),
	.mg_group_id = MGMT_GROUP_ID_SE,
};

static void se_mgmt_register_group(void)
{
	mgmt_register_group(&se_mgmt_group);
}

MCUMGR_HANDLER_DEFINE(se_mgmt, se_mgmt_register_group);