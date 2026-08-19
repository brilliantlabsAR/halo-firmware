/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <stdio.h>
#include <string.h>

#include "halo/ble_security.h"
#include "halo/ble_internal.h"
#include "halo/file_manager.h"

/* Alif BLE includes */
#include "alif_ble.h"
#include "gapm.h"
#include "gapm_le_init.h"
#include "gap_le.h"
#include "gapc.h"
#include "gapc_le.h"
#include "gapc_sec.h"
#include "co_math.h"

/* SE Service for EUI */
#include "se_service.h"

#include <zephyr/drivers/led.h>
#include <halo/led_manager.h>

LOG_MODULE_REGISTER(halo_ble_sec, CONFIG_HALO_LOG_LEVEL);

/* See PAIRING.md at the repo root for the multi-bond design this implements. */

#define BOND_SLOT_COUNT CONFIG_HALO_BLE_MAX_BONDS

/* Legacy single-bond settings keys (pre multi-bond firmware); migrated to
 * slot 0 on first boot and then deleted. */
#define LEGACY_BOND_KEYS_FILE "/lfs/ble/bond_keys"
#define LEGACY_BOND_DATA_FILE "/lfs/ble/bond_data"
#define LEGACY_BOND_ADDR_FILE "/lfs/ble/bond_addr"

#define BOND_KEY_MAX_LEN 32

/* The magic bakes in the context size so any layout change - editing the
 * struct or reconfiguring CONFIG_HALO_BLE_MAX_BONDS - invalidates noinit
 * state from a previous image, forcing a settings reload on a warm reboot
 * across an OTA boundary instead of misreading the old layout. (0x534543xx
 * base replaces the pre-multi-bond 'SECT' magic for the same reason.) */
#define BLE_SECURITY_INIT_MAGIC (0x53454300u ^ (uint32_t)sizeof(sec_ctx))

/* Persisted per-slot alongside keys/data/addr; written last so a slot only
 * counts as valid once fully persisted. */
struct bond_slot_meta {
	uint32_t last_used;
	uint8_t valid;
} __packed;

/* Security context */
static struct {
	uint32_t initialized;
	bool paired;
	bool encrypted; /* Controller reported the current link encrypted (auth_info) */
	uint8_t conidx; /* Current connection index */
	int8_t active_slot;  /* Bond slot of the current connection, -1 if none */
	bool pending_active; /* A new (not yet committed) pairing is in progress */
	uint32_t lru_counter;
	struct halo_ble_bond_info pending; /* Staging for an in-progress new pairing */
	struct halo_ble_bond_info bonds[BOND_SLOT_COUNT];
	gap_sec_key_t irk; /* Local Identity Resolving Key */
	struct k_mutex lock;
} sec_ctx __attribute__((noinit));

/* Pairing window state. Deliberately NOT in noinit: a reboot closes the
 * window, and k_work objects hold kernel pointers that must not survive one. */
static struct {
	bool open;
	bool work_initialized;
	struct k_work_delayable close_work;
} window;

#ifdef CONFIG_HALO_BLE_REQUIRE_PAIRING
/* State for the async RPA resolution: the IRK array must stay valid for the
 * call's duration and the connecting address is needed again in the callback
 * (single connection, so no overlapping resolutions). */
static gap_sec_key_t resolve_irks[BOND_SLOT_COUNT];
static gap_bdaddr_t resolving_addr;
#endif

/* Forward declarations */
static void known_device_connected(uint8_t conidx, int slot);
static void update_pairing_led(void);
#ifdef CONFIG_HALO_BLE_REQUIRE_PAIRING
static void on_address_resolved_cb(uint16_t status, const gap_addr_t *p_addr,
				   const gap_sec_key_t *pirk);
#endif

/* ---------------------------------------------------------------------- */
/* Bond slot storage                                                       */
/* ---------------------------------------------------------------------- */

static void bond_slot_key(char *buf, size_t len, int slot, const char *suffix)
{
	snprintf(buf, len, "/lfs/ble/bond%d_%s", slot, suffix);
}

/* Caller must hold sec_ctx.lock */
static int bond_slot_persist(int slot)
{
	char key[BOND_KEY_MAX_LEN];
	const struct halo_ble_bond_info *b = &sec_ctx.bonds[slot];
	struct bond_slot_meta meta = {
		.last_used = b->last_used,
		.valid = b->valid ? 1 : 0,
	};
	int ret;

	bond_slot_key(key, sizeof(key), slot, "keys");
	ret = halo_settings_set(key, &b->keys, sizeof(b->keys));
	bond_slot_key(key, sizeof(key), slot, "data");
	ret |= halo_settings_set(key, &b->data, sizeof(b->data));
	bond_slot_key(key, sizeof(key), slot, "addr");
	ret |= halo_settings_set(key, &b->peer_addr, sizeof(b->peer_addr));
	/* Meta last: it is what marks the slot valid */
	bond_slot_key(key, sizeof(key), slot, "meta");
	ret |= halo_settings_set(key, &meta, sizeof(meta));

	if (ret) {
		LOG_ERR("Failed to persist bond slot %d: %d", slot, ret);
		return -EIO;
	}
	return 0;
}

/* Caller must hold sec_ctx.lock */
static int bond_slot_persist_meta(int slot)
{
	char key[BOND_KEY_MAX_LEN];
	struct bond_slot_meta meta = {
		.last_used = sec_ctx.bonds[slot].last_used,
		.valid = sec_ctx.bonds[slot].valid ? 1 : 0,
	};

	bond_slot_key(key, sizeof(key), slot, "meta");
	return halo_settings_set(key, &meta, sizeof(meta));
}

/* Caller must hold sec_ctx.lock */
static void bond_slot_erase(int slot)
{
	char key[BOND_KEY_MAX_LEN];

	bond_slot_key(key, sizeof(key), slot, "meta");
	settings_delete(key);
	bond_slot_key(key, sizeof(key), slot, "keys");
	settings_delete(key);
	bond_slot_key(key, sizeof(key), slot, "data");
	settings_delete(key);
	bond_slot_key(key, sizeof(key), slot, "addr");
	settings_delete(key);

	memset(&sec_ctx.bonds[slot], 0, sizeof(sec_ctx.bonds[slot]));
}

/* Caller must hold sec_ctx.lock */
static void bond_slot_load(int slot)
{
	char key[BOND_KEY_MAX_LEN];
	struct halo_ble_bond_info *b = &sec_ctx.bonds[slot];
	struct bond_slot_meta meta;

	memset(b, 0, sizeof(*b));
	memset(&meta, 0, sizeof(meta));

	/* halo_settings_get() leaves the buffer untouched (and returns 0) for a
	 * missing key, so a zeroed meta means "no such slot". */
	bond_slot_key(key, sizeof(key), slot, "meta");
	halo_settings_get(key, &meta, sizeof(meta));
	if (meta.valid != 1) {
		return;
	}

	bond_slot_key(key, sizeof(key), slot, "keys");
	halo_settings_get(key, &b->keys, sizeof(b->keys));
	bond_slot_key(key, sizeof(key), slot, "data");
	halo_settings_get(key, &b->data, sizeof(b->data));
	bond_slot_key(key, sizeof(key), slot, "addr");
	halo_settings_get(key, &b->peer_addr, sizeof(b->peer_addr));

	b->last_used = meta.last_used;
	b->valid = true;
}

static bool addr_is_zero(const uint8_t *addr)
{
	for (int i = 0; i < GAP_BD_ADDR_LEN; i++) {
		if (addr[i] != 0x00) {
			return false;
		}
	}
	return true;
}

/* Import a pre-multi-bond single bond into slot 0. Caller must hold lock. */
static void migrate_legacy_bond(void)
{
	struct halo_ble_bond_info *b = &sec_ctx.bonds[0];
	gap_bdaddr_t legacy_addr;

	memset(&legacy_addr, 0, sizeof(legacy_addr));
	halo_settings_get(LEGACY_BOND_ADDR_FILE, &legacy_addr, sizeof(legacy_addr));
	if (addr_is_zero(legacy_addr.addr)) {
		return; /* No legacy bond */
	}

	if (!b->valid) {
		memset(b, 0, sizeof(*b));
		halo_settings_get(LEGACY_BOND_KEYS_FILE, &b->keys, sizeof(b->keys));
		halo_settings_get(LEGACY_BOND_DATA_FILE, &b->data, sizeof(b->data));
		b->peer_addr = legacy_addr;
		b->last_used = ++sec_ctx.lru_counter;
		b->valid = true;
		bond_slot_persist(0);
		LOG_INF("Migrated legacy bond to slot 0");
	}

	settings_delete(LEGACY_BOND_KEYS_FILE);
	settings_delete(LEGACY_BOND_DATA_FILE);
	settings_delete(LEGACY_BOND_ADDR_FILE);
}

/* Caller must hold sec_ctx.lock */
static int bond_count_locked(void)
{
	int count = 0;

	for (int i = 0; i < BOND_SLOT_COUNT; i++) {
		if (sec_ctx.bonds[i].valid) {
			count++;
		}
	}
	return count;
}

/* Caller must hold sec_ctx.lock */
static int find_slot_by_addr(const uint8_t *addr)
{
	for (int i = 0; i < BOND_SLOT_COUNT; i++) {
		if (sec_ctx.bonds[i].valid &&
		    memcmp(addr, sec_ctx.bonds[i].peer_addr.addr, GAP_BD_ADDR_LEN) == 0) {
			return i;
		}
	}
	return -1;
}

/* Caller must hold sec_ctx.lock */
static int find_slot_by_irk(const gap_sec_key_t *irk)
{
	for (int i = 0; i < BOND_SLOT_COUNT; i++) {
		if (sec_ctx.bonds[i].valid &&
		    (sec_ctx.bonds[i].keys.valid_key_bf & GAP_KDIST_IDKEY) &&
		    memcmp(irk->key, sec_ctx.bonds[i].keys.irk.key.key, GAP_KEY_LEN) == 0) {
			return i;
		}
	}
	return -1;
}

/* Free slot if any, else the least-recently-used victim. Caller must hold lock. */
static int find_commit_slot(void)
{
	int victim = 0;
	uint32_t oldest = UINT32_MAX;

	for (int i = 0; i < BOND_SLOT_COUNT; i++) {
		if (!sec_ctx.bonds[i].valid) {
			return i;
		}
		if (sec_ctx.bonds[i].last_used < oldest) {
			oldest = sec_ctx.bonds[i].last_used;
			victim = i;
		}
	}
	LOG_INF("Bond table full - evicting LRU slot %d", victim);
	return victim;
}

/* Caller must hold sec_ctx.lock */
static void touch_slot(int slot)
{
	sec_ctx.bonds[slot].last_used = ++sec_ctx.lru_counter;
	bond_slot_persist_meta(slot);
}

/* ---------------------------------------------------------------------- */
/* Pairing window                                                          */
/* ---------------------------------------------------------------------- */

/* Pairable = window open, or nothing bonded yet (out-of-box behaviour). */
static bool pairable(void)
{
#ifdef CONFIG_HALO_BLE_ALLOW_BOND_OVERWRITE
	return true;
#else
	if (window.open) {
		return true;
	}

	k_mutex_lock(&sec_ctx.lock, K_FOREVER);
	bool no_bonds = (bond_count_locked() == 0);
	k_mutex_unlock(&sec_ctx.lock);

	return no_bonds;
#endif
}

static void update_pairing_led(void)
{
#ifdef CONFIG_HALO_LED_MANAGER
	if (pairable()) {
		halo_led_set_state(HALO_LED_STATE_PAIRING, HALO_LED_PRIORITY_HIGH);
	} else {
		halo_led_clear_state(HALO_LED_PRIORITY_HIGH);
	}
#endif
}

static void pairing_window_close_work(struct k_work *work)
{
	ARG_UNUSED(work);

	if (window.open) {
		LOG_INF("Pairing window timed out");
		window.open = false;
		update_pairing_led();
	}
}

int halo_ble_sec_pairing_window_open(void)
{
	if (sec_ctx.initialized != BLE_SECURITY_INIT_MAGIC) {
		return -ENODEV;
	}

	if (!window.work_initialized) {
		k_work_init_delayable(&window.close_work, pairing_window_close_work);
		window.work_initialized = true;
	}

	window.open = true;
	k_work_reschedule(&window.close_work, K_SECONDS(CONFIG_HALO_BLE_PAIRING_WINDOW_SEC));
	update_pairing_led();

	LOG_INF("Pairing window open for %d s", CONFIG_HALO_BLE_PAIRING_WINDOW_SEC);
	return 0;
}

bool halo_ble_sec_pairing_window_is_open(void)
{
	return window.open;
}

/* Called once a new bond is committed */
static void pairing_window_close(void)
{
	if (window.open) {
		window.open = false;
		if (window.work_initialized) {
			k_work_cancel_delayable(&window.close_work);
		}
	}
}

/* ---------------------------------------------------------------------- */
/* Init                                                                    */
/* ---------------------------------------------------------------------- */

int halo_ble_sec_init(void)
{
	if (sec_ctx.initialized == BLE_SECURITY_INIT_MAGIC) {
		/* Warm reboot: bond table survived in noinit RAM */
		update_pairing_led();
		return 0;
	}

	k_mutex_init(&sec_ctx.lock);

	/* Generate Local IRK from device EUI-64 for privacy address generation */
	uint8_t eui[8];
	int ret;

	/* Get complete 8-byte EUI-64: OUI (2C:F7:F1) + 5-byte extension */
	ret = se_system_get_eui_extension(false, &eui[3]);
	if (ret == 0) {
		eui[0] = 0x2C;
		eui[1] = 0xF7;
		eui[2] = 0xF1;

		/* Derive 16-byte IRK from 8-byte EUI-64 */
		/* Method: EUI-64 forward + EUI-64 reversed (provides good mixing) */
		memcpy(&sec_ctx.irk.key[0], eui, 8);  /* Bytes 0-7: forward */
		for (int i = 0; i < 8; i++) {
			sec_ctx.irk.key[8 + i] = eui[7 - i];  /* Bytes 8-15: reversed */
		}

		LOG_DBG("Local IRK generated from EUI-64: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
			eui[0], eui[1], eui[2], eui[3], eui[4], eui[5], eui[6], eui[7]);
	} else {
		LOG_WRN("Failed to get EUI-64, using static fallback IRK: %d", ret);
		/* Fallback: use static IRK to maintain consistency across reboots */
		static const uint8_t fallback_irk[GAP_KEY_LEN] = {
			0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
			0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10
		};
		memcpy(sec_ctx.irk.key, fallback_irk, GAP_KEY_LEN);
	}

	k_mutex_lock(&sec_ctx.lock, K_FOREVER);

	sec_ctx.paired = false;
	sec_ctx.encrypted = false;
	sec_ctx.active_slot = -1;
	sec_ctx.pending_active = false;
	sec_ctx.lru_counter = 0;
	memset(&sec_ctx.pending, 0, sizeof(sec_ctx.pending));

	for (int i = 0; i < BOND_SLOT_COUNT; i++) {
		bond_slot_load(i);
		if (sec_ctx.bonds[i].valid && sec_ctx.bonds[i].last_used > sec_ctx.lru_counter) {
			sec_ctx.lru_counter = sec_ctx.bonds[i].last_used;
		}
	}

	migrate_legacy_bond();

	int count = bond_count_locked();

	k_mutex_unlock(&sec_ctx.lock);

	LOG_INF("Bond table: %d/%d slots used", count, BOND_SLOT_COUNT);

	sec_ctx.initialized = BLE_SECURITY_INIT_MAGIC;

	update_pairing_led();
	return 0;
}

/* ---------------------------------------------------------------------- */
/* Pairing procedure                                                       */
/* ---------------------------------------------------------------------- */

int halo_ble_sec_pair_request(uint8_t conidx, uint8_t auth_level)
{
	int err;

	if (sec_ctx.initialized != BLE_SECURITY_INIT_MAGIC) {
		return -ENODEV;
	}

	/* Initiate pairing using Alif BLE API */
	err = gapc_le_request_security(conidx, auth_level);
	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("gapc_le_request_security failed: %u", err);
		return -EIO;
	}

	return 0;
}

int halo_ble_sec_pair_accept(uint8_t conidx)
{
	int err;
	gapc_pairing_t pairing_info;

	if (sec_ctx.initialized != BLE_SECURITY_INIT_MAGIC) {
		return -ENODEV;
	}

	/* Set up pairing parameters */
	pairing_info.auth = GAP_AUTH_REQ_SEC_CON_BOND;
	pairing_info.iocap = GAP_IO_CAP_NO_INPUT_NO_OUTPUT;
	pairing_info.ikey_dist = GAP_KDIST_ENCKEY | GAP_KDIST_IDKEY;
	pairing_info.rkey_dist = GAP_KDIST_ENCKEY | GAP_KDIST_IDKEY;
	pairing_info.key_size = GAP_KEY_LEN;
	pairing_info.oob = GAP_OOB_AUTH_DATA_NOT_PRESENT;

	/* Accept pairing using Alif BLE API */
	err = gapc_le_pairing_accept(conidx, true, &pairing_info, 0);
	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("gapc_le_pairing_accept failed: %u", err);
		return -EIO;
	}

	return 0;
}

int halo_ble_sec_pair_reject(uint8_t conidx)
{
	int err;

	if (sec_ctx.initialized != BLE_SECURITY_INIT_MAGIC) {
		return -ENODEV;
	}

	/* Reject pairing using Alif BLE API */
	err = gapc_le_pairing_accept(conidx, false, NULL, 0);
	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("gapc_le_pairing_accept (reject) failed: %u", err);
		return -EIO;
	}

	return 0;
}

int halo_ble_sec_bond_clear(void)
{
	if (sec_ctx.initialized != BLE_SECURITY_INIT_MAGIC) {
		return -ENODEV;
	}

	k_mutex_lock(&sec_ctx.lock, K_FOREVER);

	for (int i = 0; i < BOND_SLOT_COUNT; i++) {
		bond_slot_erase(i);
	}
	memset(&sec_ctx.pending, 0, sizeof(sec_ctx.pending));
	sec_ctx.pending_active = false;
	sec_ctx.active_slot = -1;
	sec_ctx.paired = false;
	sec_ctx.encrypted = false;

	k_mutex_unlock(&sec_ctx.lock);

	update_pairing_led();

	/* Notify unpaired event */
	struct halo_ble_event_data event = {
		.event = HALO_BLE_EVENT_UNPAIRED,
		.conidx = 0xFF,
	};
	halo_ble_notify_event(&event);

	return 0;
}

bool halo_ble_sec_is_bonded(void)
{
	bool bonded;

	if (sec_ctx.initialized != BLE_SECURITY_INIT_MAGIC) {
		return false;
	}

	k_mutex_lock(&sec_ctx.lock, K_FOREVER);
	bonded = (bond_count_locked() > 0);
	k_mutex_unlock(&sec_ctx.lock);

	return bonded;
}

bool halo_ble_sec_is_paired(void)
{
	bool paired;

	if (sec_ctx.initialized != BLE_SECURITY_INIT_MAGIC) {
		return false;
	}

	k_mutex_lock(&sec_ctx.lock, K_FOREVER);
	paired = sec_ctx.paired;
	k_mutex_unlock(&sec_ctx.lock);

	return paired;
}

bool halo_ble_sec_is_encrypted(void)
{
	bool encrypted;

	if (sec_ctx.initialized != BLE_SECURITY_INIT_MAGIC) {
		return false;
	}

	k_mutex_lock(&sec_ctx.lock, K_FOREVER);
	encrypted = sec_ctx.encrypted;
	k_mutex_unlock(&sec_ctx.lock);

	return encrypted;
}

bool halo_ble_sec_can_pair(void)
{
	if (sec_ctx.initialized != BLE_SECURITY_INIT_MAGIC) {
		return false;
	}

	return pairable();
}

void halo_ble_sec_on_paired(uint8_t conidx, uint32_t metainfo, uint8_t pairing_level,
			    bool enc_key_present, uint8_t key_type)
{
	if (sec_ctx.initialized != BLE_SECURITY_INIT_MAGIC) {
		return;
	}

	k_mutex_lock(&sec_ctx.lock, K_FOREVER);

	if (sec_ctx.pending_active) {
		/* Fresh pairing of a new device: commit the staged bond */
		int slot = find_commit_slot();

		sec_ctx.pending.data.pairing_lvl = pairing_level;
		sec_ctx.pending.data.enc_key_present = enc_key_present;
		sec_ctx.pending.last_used = ++sec_ctx.lru_counter;
		sec_ctx.pending.valid = true;

		memcpy(&sec_ctx.bonds[slot], &sec_ctx.pending, sizeof(sec_ctx.pending));
		memset(&sec_ctx.pending, 0, sizeof(sec_ctx.pending));
		sec_ctx.pending_active = false;
		sec_ctx.active_slot = slot;

		bond_slot_persist(slot);
		LOG_INF("New bond committed to slot %d", slot);
	} else if (sec_ctx.active_slot >= 0) {
		/* Re-pairing (or data refresh) by an already-bonded peer */
		int slot = sec_ctx.active_slot;

		sec_ctx.bonds[slot].data.pairing_lvl = pairing_level;
		sec_ctx.bonds[slot].data.enc_key_present = enc_key_present;
		bond_slot_persist(slot);
	}

	if (gapc_is_bonded(conidx)) {
		sec_ctx.paired = true;
	}

	k_mutex_unlock(&sec_ctx.lock);

	pairing_window_close();
	update_pairing_led();

	/* Notify event */
	struct halo_ble_event_data event = {
		.event = HALO_BLE_EVENT_PAIRED,
		.conidx = conidx,
	};
	halo_ble_notify_event(&event);
}

void halo_ble_sec_on_pair_failed(uint8_t conidx, uint16_t reason)
{
	if (sec_ctx.initialized != BLE_SECURITY_INIT_MAGIC) {
		return;
	}

	LOG_ERR("Pairing failed: conidx=%u, reason=0x%04x", conidx, reason);

	k_mutex_lock(&sec_ctx.lock, K_FOREVER);
	sec_ctx.paired = false;
	sec_ctx.encrypted = false;

	if (sec_ctx.pending_active) {
		/* Discard the staged pairing; committed bonds are untouched */
		memset(&sec_ctx.pending, 0, sizeof(sec_ctx.pending));
		sec_ctx.pending_active = false;
	} else if (sec_ctx.active_slot >= 0) {
		/* Known peer failed re-pairing - likely transient, keep its bond */
		LOG_WRN("Keeping bond slot %d after transient pairing failure",
			sec_ctx.active_slot);
	}

	k_mutex_unlock(&sec_ctx.lock);

	update_pairing_led();
}

void halo_ble_sec_on_encrypted(uint8_t conidx, uint8_t sec_lvl, uint8_t key_size)
{
	if (sec_ctx.initialized != BLE_SECURITY_INIT_MAGIC) {
		return;
	}

	k_mutex_lock(&sec_ctx.lock, K_FOREVER);
	sec_ctx.encrypted = true;
	k_mutex_unlock(&sec_ctx.lock);

	LOG_INF("Link encrypted: conidx=%u, sec_lvl=%u, key_size=%u", conidx, sec_lvl, key_size);

	/* Notify: unlike PAIRED (set optimistically before encryption on a bonded
	 * reconnect), this fires only once the link is actually encrypted. */
	struct halo_ble_event_data event = {
		.event = HALO_BLE_EVENT_ENCRYPTED,
		.conidx = conidx,
	};
	halo_ble_notify_event(&event);
}

void halo_ble_sec_on_encrypt_failed(uint8_t conidx, uint16_t reason)
{
	if (sec_ctx.initialized != BLE_SECURITY_INIT_MAGIC) {
		return;
	}

	k_mutex_lock(&sec_ctx.lock, K_FOREVER);
	sec_ctx.paired = false;
	sec_ctx.encrypted = false;

	/* Do NOT erase the bond here. This path is reached for any peer that
	 * connected as a known identity address but could not encrypt - which an
	 * attacker can force by spoofing a bonded peer's (public/static) identity
	 * address with no knowledge of its keys. Erasing on that unauthenticated
	 * failure let such a peer delete a legitimate user's bond (DoS forcing a
	 * re-pair). A peer that genuinely lost its keys recovers by re-pairing
	 * (on_gapc_pairing_req), which refreshes the slot in place - no erase is
	 * required for the legitimate case. */
	LOG_ERR("Encryption failed: conidx=%u, reason=0x%04x (bond slot %d kept)", conidx, reason,
		sec_ctx.active_slot);
	sec_ctx.active_slot = -1;

	k_mutex_unlock(&sec_ctx.lock);

	update_pairing_led();

	/* Disconnect to allow fresh pairing attempt */
	gapc_disconnect(conidx, 0, reason, NULL);
}

int halo_ble_sec_on_encrypt_req(uint8_t conidx)
{
	int err;

	if (sec_ctx.initialized != BLE_SECURITY_INIT_MAGIC) {
		return -ENODEV;
	}

	k_mutex_lock(&sec_ctx.lock, K_FOREVER);

	int slot = sec_ctx.active_slot;

	/* Check if we have a valid LTK for the connected peer */
	if (slot < 0 || !sec_ctx.bonds[slot].valid ||
	    !(sec_ctx.bonds[slot].keys.valid_key_bf & GAP_KDIST_ENCKEY)) {
		k_mutex_unlock(&sec_ctx.lock);
		LOG_WRN("No valid LTK available, rejecting encryption request");
		/* Reject encryption if no valid key */
		gapc_le_encrypt_req_reply(conidx, false, NULL, 0);
		return -EIO;
	}

	/* Reply with LTK using Alif BLE API */
	err = gapc_le_encrypt_req_reply(conidx, true, &sec_ctx.bonds[slot].keys.ltk.key,
					sec_ctx.bonds[slot].keys.ltk.key_size);

	k_mutex_unlock(&sec_ctx.lock);

	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("gapc_le_encrypt_req_reply failed: %u", err);
		return -EIO;
	}

	return 0;
}

void halo_ble_sec_on_keys_received(uint8_t conidx, const gapc_pairing_keys_t *p_keys)
{
	if (sec_ctx.initialized != BLE_SECURITY_INIT_MAGIC || !p_keys) {
		return;
	}

	k_mutex_lock(&sec_ctx.lock, K_FOREVER);

	/* New pairings accumulate in the staging bond; a known peer re-pairing
	 * refreshes its own slot in place. */
	struct halo_ble_bond_info *b;

	if (sec_ctx.pending_active) {
		b = &sec_ctx.pending;
	} else if (sec_ctx.active_slot >= 0) {
		b = &sec_ctx.bonds[sec_ctx.active_slot];
	} else {
		k_mutex_unlock(&sec_ctx.lock);
		LOG_WRN("Keys received with no pairing in progress - ignored");
		return;
	}

	uint8_t key_bits = GAP_KDIST_NONE;

	if (p_keys->valid_key_bf & GAP_KDIST_ENCKEY) {
		memcpy(&b->keys.ltk, &p_keys->ltk, sizeof(b->keys.ltk));
		key_bits |= GAP_KDIST_ENCKEY;
	}

	if (p_keys->valid_key_bf & GAP_KDIST_IDKEY) {
		memcpy(&b->keys.irk, &p_keys->irk, sizeof(b->keys.irk));
		key_bits |= GAP_KDIST_IDKEY;

		/* Prefer the peer's identity address over the (possibly
		 * rotating) connection address for slot matching. */
		if (!addr_is_zero(p_keys->irk.identity.addr)) {
			b->peer_addr = p_keys->irk.identity;
		}
	}

	if (p_keys->valid_key_bf & GAP_KDIST_SIGNKEY) {
		memcpy(&b->keys.csrk, &p_keys->csrk, sizeof(b->keys.csrk));
		key_bits |= GAP_KDIST_SIGNKEY;
	}

	b->keys.pairing_lvl = p_keys->pairing_lvl;
	b->keys.valid_key_bf |= key_bits;

	/* Persist immediately only for in-place refresh of an existing slot;
	 * staged pairings are persisted at commit time in on_paired(). */
	if (!sec_ctx.pending_active && sec_ctx.active_slot >= 0) {
		bond_slot_persist(sec_ctx.active_slot);
	}

	k_mutex_unlock(&sec_ctx.lock);
}

int halo_ble_sec_provide_irk(uint8_t conidx)
{
	int err;

	if (sec_ctx.initialized != BLE_SECURITY_INIT_MAGIC) {
		return -ENODEV;
	}

	k_mutex_lock(&sec_ctx.lock, K_FOREVER);

	/* Provide IRK using Alif BLE API */
	err = gapc_le_pairing_provide_irk(conidx, &sec_ctx.irk);

	k_mutex_unlock(&sec_ctx.lock);

	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("gapc_le_pairing_provide_irk failed: %u", err);
		return -EIO;
	}

	return 0;
}

int halo_ble_sec_provide_csrk(uint8_t conidx)
{
	int err;

	if (sec_ctx.initialized != BLE_SECURITY_INIT_MAGIC) {
		return -ENODEV;
	}

	k_mutex_lock(&sec_ctx.lock, K_FOREVER);

	gapc_bond_data_t *data = sec_ctx.pending_active ? &sec_ctx.pending.data
			: (sec_ctx.active_slot >= 0 ? &sec_ctx.bonds[sec_ctx.active_slot].data
						    : &sec_ctx.pending.data);

	/* Provide CSRK using Alif BLE API */
	err = gapc_pairing_provide_csrk(conidx, &data->local_csrk);

	k_mutex_unlock(&sec_ctx.lock);

	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("gapc_pairing_provide_csrk failed: %u", err);
		return -EIO;
	}

	return 0;
}

int halo_ble_sec_provide_ltk(uint8_t conidx, uint8_t key_size)
{
	int err;
	uint8_t cnt;

	if (sec_ctx.initialized != BLE_SECURITY_INIT_MAGIC) {
		return -ENODEV;
	}

	k_mutex_lock(&sec_ctx.lock, K_FOREVER);

	gapc_ltk_t ltk_data;

	/* Generate new LTK */
	ltk_data.key_size = GAP_KEY_LEN;
	ltk_data.ediv = (uint16_t)co_rand_word();

	for (cnt = 0; cnt < RAND_NB_LEN; cnt++) {
		ltk_data.key.key[cnt] = (uint8_t)co_rand_word();
		ltk_data.randnb.nb[cnt] = (uint8_t)co_rand_word();
	}

	for (cnt = RAND_NB_LEN; cnt < GAP_KEY_LEN; cnt++) {
		ltk_data.key.key[cnt] = (uint8_t)co_rand_word();
	}

	/* Provide LTK using Alif BLE API */
	err = gapc_le_pairing_provide_ltk(conidx, &ltk_data);

	if (err == GAP_ERR_NO_ERROR) {
		struct halo_ble_bond_info *b =
			sec_ctx.pending_active ? &sec_ctx.pending
			: (sec_ctx.active_slot >= 0 ? &sec_ctx.bonds[sec_ctx.active_slot]
						    : NULL);

		if (b) {
			memcpy(&b->keys.ltk, &ltk_data, sizeof(gapc_ltk_t));
			b->keys.valid_key_bf |= GAP_KDIST_ENCKEY;
			b->keys.pairing_lvl = GAP_PAIRING_BOND_AUTH;

			if (!sec_ctx.pending_active && sec_ctx.active_slot >= 0) {
				bond_slot_persist(sec_ctx.active_slot);
			}
		}
	}

	k_mutex_unlock(&sec_ctx.lock);

	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("gapc_le_pairing_provide_ltk failed: %u", err);
		return -EIO;
	}

	return 0;
}

/* ---------------------------------------------------------------------- */
/* Connection acceptance                                                   */
/* ---------------------------------------------------------------------- */

/* A bonded peer connected and was identified as `slot`. */
static void known_device_connected(uint8_t conidx, int slot)
{
	k_mutex_lock(&sec_ctx.lock, K_FOREVER);
	sec_ctx.active_slot = slot;
	sec_ctx.paired = true;
	touch_slot(slot);
	gapc_bond_data_t bond_data = sec_ctx.bonds[slot].data;
	k_mutex_unlock(&sec_ctx.lock);

	uint16_t status = gapc_le_connection_cfm(conidx, 0, &bond_data);
	if (status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Connection confirmation with bond data failed: 0x%04x", status);
		return;
	}

	LOG_INF("Bonded device (slot %d) reconnected - requesting encryption", slot);

	/* Request encryption to verify bond is still valid */
	uint16_t enc_err = gapc_le_request_security(conidx, GAP_AUTH_SEC_CON);
	if (enc_err != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to request encryption: 0x%04x", enc_err);
		/* Continue anyway - encryption failure will be caught later */
	}

	/* Notify paired event for bonded device reconnection */
	struct halo_ble_event_data event = {
		.event = HALO_BLE_EVENT_PAIRED,
		.conidx = conidx,
	};
	halo_ble_notify_event(&event);
}

#ifdef CONFIG_HALO_BLE_REQUIRE_PAIRING
/* An unknown peer connected while pairable: accept and stage a new pairing. */
static void unknown_device_connected(uint8_t conidx, const gap_bdaddr_t *p_peer_addr)
{
	k_mutex_lock(&sec_ctx.lock, K_FOREVER);
	sec_ctx.active_slot = -1;
	memset(&sec_ctx.pending, 0, sizeof(sec_ctx.pending));
	memcpy(&sec_ctx.pending.peer_addr, p_peer_addr, sizeof(gap_bdaddr_t));
	sec_ctx.pending_active = true;
	k_mutex_unlock(&sec_ctx.lock);

	uint16_t cfm_status = gapc_le_connection_cfm(conidx, 0, NULL);
	if (cfm_status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Connection confirmation for new device failed: 0x%04x", cfm_status);
		return;
	}

#if defined(CONFIG_HALO_BLE_REQUIRE_PAIRING) && !defined(CONFIG_MCUBOOT)
	/* Request pairing for new device */
	uint16_t err = gapc_le_request_security(conidx, GAP_AUTH_REQ_SEC_CON_BOND);
	if (err != GAP_ERR_NO_ERROR) {
		LOG_ERR("Failed to request security: %u", err);
	}
#endif
}

/* Reject: peer is neither bonded nor allowed to pair right now, or is bonded
 * but the pairing window must stay free for a new device. */
static void reject_device(uint8_t conidx, uint16_t reason, const char *why)
{
	LOG_WRN("Rejecting connection - %s", why);
	gapc_disconnect(conidx, 0, reason, NULL);
}

/* Dispatch an identified (or unidentified) peer per the PAIRING.md matrix. */
static int dispatch_connection(uint8_t conidx, int slot, const gap_bdaddr_t *p_peer_addr)
{
	if (slot >= 0) {
		if (halo_ble_sec_pairing_window_is_open()) {
			/* Single connection: a bonded peer would occupy the link
			 * and starve the new device the window exists for. */
			reject_device(conidx, LL_ERR_REMOTE_USER_TERM_CON,
				      "bonded peer during pairing window");
			return -EACCES;
		}
		known_device_connected(conidx, slot);
		return 0;
	}

	if (!pairable()) {
		reject_device(conidx, LL_ERR_AUTH_FAILURE,
			      "unknown device and not in pairing mode");
		return -EACCES;
	}

	unknown_device_connected(conidx, p_peer_addr);
	return 0;
}

/* Address resolution callback */
static void on_address_resolved_cb(uint16_t status, const gap_addr_t *p_addr,
				   const gap_sec_key_t *pirk)
{
	ARG_UNUSED(p_addr);

	uint8_t conidx = sec_ctx.conidx;
	int slot = -1;

	if (status == GAP_ERR_NO_ERROR && pirk != NULL) {
		k_mutex_lock(&sec_ctx.lock, K_FOREVER);
		slot = find_slot_by_irk(pirk);
		k_mutex_unlock(&sec_ctx.lock);
	}

	dispatch_connection(conidx, slot, &resolving_addr);
}
#endif /* CONFIG_HALO_BLE_REQUIRE_PAIRING */

int halo_ble_sec_on_connection_req(uint8_t conidx, const gap_bdaddr_t *p_peer_addr)
{
	if (!p_peer_addr) {
		return -EINVAL;
	}

	if (sec_ctx.initialized != BLE_SECURITY_INIT_MAGIC) {
		return -ENODEV;
	}

	k_mutex_lock(&sec_ctx.lock, K_FOREVER);

	sec_ctx.conidx = conidx;
	sec_ctx.active_slot = -1;
	sec_ctx.pending_active = false;
	sec_ctx.paired = false;
	sec_ctx.encrypted = false;

	/* Direct match covers public and static-random peers, plus an RPA that
	 * happens to equal a stored identity address. */
	int slot = find_slot_by_addr(p_peer_addr->addr);

#ifdef CONFIG_HALO_BLE_REQUIRE_PAIRING
	/* Gather bonded IRKs for RPA resolution */
	uint8_t nb_irk = 0;

	if (slot < 0 && p_peer_addr->addr_type != GAP_ADDR_PUBLIC) {
		for (int i = 0; i < BOND_SLOT_COUNT; i++) {
			if (sec_ctx.bonds[i].valid &&
			    (sec_ctx.bonds[i].keys.valid_key_bf & GAP_KDIST_IDKEY)) {
				memcpy(&resolve_irks[nb_irk],
				       &sec_ctx.bonds[i].keys.irk.key,
				       sizeof(gap_sec_key_t));
				nb_irk++;
			}
		}
	}
#endif

	k_mutex_unlock(&sec_ctx.lock);

#ifdef CONFIG_HALO_BLE_REQUIRE_PAIRING
	if (slot < 0 && nb_irk > 0) {
		/* Random address: identify the peer by resolving against all
		 * bonded IRKs; the callback dispatches the connection. */
		resolving_addr = *p_peer_addr;
		uint16_t status = gapm_le_resolve_address((gap_addr_t *)p_peer_addr->addr,
							  nb_irk, resolve_irks,
							  on_address_resolved_cb);
		if (status == GAP_ERR_NO_ERROR) {
			return 0; /* Resolution in progress */
		}
		if (status != GAP_ERR_INVALID_PARAM) {
			LOG_ERR("Failed to start address resolution: %u", status);
			return -EIO;
		}
		/* GAP_ERR_INVALID_PARAM: address is not resolvable - fall
		 * through and treat as unknown. */
		LOG_DBG("Address not resolvable - treating as new device");
	}

	return dispatch_connection(conidx, slot, p_peer_addr);
#else
	/* Pairing not required (development): identify known peers so their
	 * LTK is available, but accept everyone without requesting security. */
	if (slot >= 0) {
		known_device_connected(conidx, slot);
		return 0;
	}

	uint16_t cfm_status = gapc_le_connection_cfm(conidx, 0, NULL);
	if (cfm_status != GAP_ERR_NO_ERROR) {
		LOG_ERR("Connection confirmation failed: 0x%04x", cfm_status);
		return -EIO;
	}

	return 0;
#endif /* CONFIG_HALO_BLE_REQUIRE_PAIRING */
}
