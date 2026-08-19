/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "gaf_scan.h"
#include "gap_le.h"
#include "gapm.h"
#include "scan.h"
#include "main.h"

#define SCAN_DURATION_SEC 10
/** See enum gaf_scan_cfg_bf for more information */
#define SCAN_CONFIG_BITS  (GAF_SCAN_CFG_ASCS_REQ_BIT /*| GAF_SCAN_CFG_TMAS_REQ_BIT*/)

LOG_MODULE_REGISTER(scan, CONFIG_UNICAST_SCAN_LOG_LEVEL);

/** Peripheral info */
struct peripheral {
	sys_snode_t node;
	/** Pointer to device address */
	gap_bdaddr_t addr;
	/** Device name */
	char device_name[32];
	/** Pointer to RSI value */
	atc_csis_rsi_t rsi;
	/** Info flag bits */
	uint8_t info_flags;
};

static sys_slist_t scan_results;
static bool scan_ongoing;
static scanning_ready_callback_t scanning_ready_cb;

const char peripheral_name[] = CONFIG_PERIPHERAL_NAME;

const char *bdaddr_str(const gap_bdaddr_t * const p_addr)
{
	static char addr_str[32];

	snprintk(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X (%u)", p_addr->addr[5],
		 p_addr->addr[4], p_addr->addr[3], p_addr->addr[2], p_addr->addr[1],
		 p_addr->addr[0], p_addr->addr_type);
	return addr_str;
}

static void *get_peripheral_by_bdaddr(gap_bdaddr_t const *const p_addr)
{
	if (!p_addr) {
		return NULL;
	}

	struct peripheral *peripheral;
	sys_snode_t *node = NULL;

	SYS_SLIST_ITERATE_FROM_NODE(&scan_results, node)
	{
		peripheral = (struct peripheral *)node;
		if (!memcmp(peripheral->addr.addr, p_addr->addr, sizeof(p_addr->addr))) {
			return peripheral;
		}
	}

	return NULL;
}

/* ---------------------------------------------------------------------------------------- */
/** GAF Client callbacks */

static void on_gaf_scanning_cb_cmp_evt(uint8_t const cmd_type, uint16_t const status)
{
	__ASSERT(status == GAF_ERR_NO_ERROR, "status %u, cmd_type %u", status, cmd_type);
	if (cmd_type == GAF_SCAN_CMD_TYPE_STOP) {
		LOG_INF("Scan stopped");
		set_blue_led(false);
		scan_ongoing = false;

		if (scanning_ready_cb) {
			scanning_ready_cb();
			scanning_ready_cb = NULL;
		}
	} else if (cmd_type == GAF_SCAN_CMD_TYPE_START) {
		LOG_INF("Scan started. Scanning for %u seconds", SCAN_DURATION_SEC);
		set_blue_led(true);
	} else {
		LOG_ERR("Unexpected GAF scanning command complete event: %u", cmd_type);
	}
}

static void on_gaf_scanning_cb_stopped(uint8_t const reason)
{
	static const char *const reason_str[] = {
		"Requested by Upper Layer",
		"Internal error",
		"Timeout",
	};

	LOG_DBG("GAF scanning stopped. Reason: %s",
		reason < ARRAY_SIZE(reason_str) ? reason_str[reason] : "??");

	if (reason == GAF_SCAN_STOP_REASON_UL) {
		return;
	}

	if (!sys_slist_is_empty(&scan_results)) {
		LOG_DBG("Scan results not empty. Not restarting scan");
		set_blue_led(false);
		scan_ongoing = false;
		if (scanning_ready_cb) {
			scanning_ready_cb();
			scanning_ready_cb = NULL;
		}
		return;
	}

	LOG_INF("restarting scan...");
	/* TODO / FIXME: Assert after 12 scan rounds! */
	uint16_t status = gaf_scan_start(SCAN_CONFIG_BITS, SCAN_DURATION_SEC, GAP_PHY_1MBPS);

	if (status != GAF_ERR_NO_ERROR) {
		__ASSERT(0, "Error %u starting scan", status);
		LOG_ERR("Failed to start scan. error %u", status);
		scan_ongoing = false;
	}
}

static void on_gaf_scanning_cb_report(const gap_bdaddr_t *p_addr, uint8_t const info_bf,
				      const gaf_adv_report_air_info_t *p_air_info,
				      uint8_t const flags, uint16_t const appearance,
				      uint16_t const tmap_roles, const atc_csis_rsi_t *p_rsi,
				      uint16_t const length, const uint8_t *p_data)
{
	const uint8_t *p_reported_name;
	uint8_t name_length = 0;

	p_reported_name =
		gapm_get_ltv_value(GAP_AD_TYPE_SHORTENED_NAME, length, p_data, &name_length);
	if (!p_reported_name) {
		p_reported_name =
			gapm_get_ltv_value(GAP_AD_TYPE_COMPLETE_NAME, length, p_data, &name_length);
	}

	if (!p_reported_name || !name_length) {
		return;
	}

	/* Check that peripheral name matches */
	if (memcmp(p_reported_name, peripheral_name, sizeof(peripheral_name) - 1)) {
		return;
	}

	/* Check if the peripheral is already on the list */
	struct peripheral *peripheral = get_peripheral_by_bdaddr(p_addr);

	if (peripheral) {
		/* Already on the list */
		return;
	}

	peripheral = malloc(sizeof(struct peripheral));
	if (!peripheral) {
		LOG_ERR("Failed to allocate memory for peripheral");
		return;
	}

	memcpy(peripheral->device_name, p_reported_name, name_length);
	peripheral->device_name[name_length] = 0;
	peripheral->addr = *p_addr;
	peripheral->info_flags = info_bf;

	if (info_bf & GAF_SCAN_REPORT_INFO_RSI_BIT) {
		memcpy(&peripheral->rsi, p_rsi, sizeof(peripheral->rsi));
	}

	LOG_INF("Device found, name: %s, addr: %s", peripheral->device_name,
		bdaddr_str(&peripheral->addr));

	sys_slist_append(&scan_results, &peripheral->node);
}

static void on_gaf_scanning_cb_announcement(const gap_bdaddr_t *const p_addr, uint8_t const type_bf,
					    uint32_t const context_bf,
					    const gaf_ltv_t *const p_metadata)
{
	const struct peripheral *const peripheral = get_peripheral_by_bdaddr(p_addr);

	if (!peripheral) {
		return;
	}

	LOG_INF("Announcement received for peripheral %s", peripheral->device_name);
	peer_found(p_addr);
}

static const struct gaf_scan_cb gaf_scan_callbacks = {
	.cb_cmp_evt = on_gaf_scanning_cb_cmp_evt,
	.cb_stopped = on_gaf_scanning_cb_stopped,
	.cb_report = on_gaf_scanning_cb_report,
	.cb_announcement = on_gaf_scanning_cb_announcement,
};

int unicast_source_scan_configure(void)
{
	sys_slist_init(&scan_results);

	uint16_t const err = gaf_scan_configure(&gaf_scan_callbacks);

	if (err != GAF_ERR_NO_ERROR) {
		LOG_ERR("GAF scanning configure error: %u", err);
		return -ENOEXEC;
	}

	LOG_DBG("Scan configured");
	return 0;
}

int unicast_source_scan_start(scanning_ready_callback_t const ready_cb)
{
	void *node = NULL;

	if (scan_ongoing) {
		return -EBUSY;
	}

	/* Cleanup the client list */
	while ((node = sys_slist_get(&scan_results))) {
		free(node);
	}

	scanning_ready_cb = ready_cb;

	uint16_t const err = gaf_scan_start(SCAN_CONFIG_BITS, SCAN_DURATION_SEC, GAP_PHY_1MBPS);

	if (err != GAF_ERR_NO_ERROR) {
		LOG_ERR("GAF scanning start error: %u", err);
		return -ENOEXEC;
	}

	scan_ongoing = true;

	return 0;
}

int unicast_source_scan_stop(void)
{
	if (!scan_ongoing) {
		return 0;
	}

	uint16_t const err = gaf_scan_stop();

	if (err != GAF_ERR_NO_ERROR) {
		LOG_ERR("GAF scanning stop error: %u", err);
		return -ENOEXEC;
	}

	return 0;
}
