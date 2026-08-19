/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * frame.ancs - Lua bindings for the Apple Notification Center Service client.
 *
 * BLE-thread events (Notification Source packets, attribute responses,
 * availability changes) are queued here and drained on the Lua REPL thread
 * via the lua_sethook mechanism, following the pattern used by
 * frame.bluetooth.receive_callback / frame.imu.tap_callback.
 *
 * A k_spinlock protects the queues. The Lua thread never calls into the
 * BLE ANCS module while holding it, and the BLE thread only holds it for
 * short copies, so there is no lock-order interaction with the ANCS
 * module's own mutex.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "lua.h"
#include "lauxlib.h"

#include <halo/lua_ancs.h>
#include <halo/lua_service.h>
#include <halo/lua_runtime.h>
#include <halo/ble_ancs.h>

LOG_MODULE_REGISTER(lua_ancs, CONFIG_HALO_LOG_LEVEL);

/* Default attribute request when no options table is given */
#define ANCS_DEFAULT_TITLE_MAX   64
#define ANCS_DEFAULT_MESSAGE_MAX 256

#define ANCS_NOTIF_QUEUE_LEN CONFIG_HALO_BLE_ANCS_NOTIF_QUEUE_LEN
#define ANCS_RESULT_QUEUE_LEN 2
#define ANCS_ACTION_QUEUE_LEN 4
#define ANCS_APP_ID_BUF_LEN   161 /* matches ANCS_APP_ID_MAX_LEN + NUL */

/* One buffered Get Notification Attributes result */
struct attr_result_entry {
	uint32_t uid;
	int status;
	uint8_t count;
	uint8_t ids[HALO_ANCS_ATTR_COUNT];
	uint16_t offsets[HALO_ANCS_ATTR_COUNT];
	uint16_t lengths[HALO_ANCS_ATTR_COUNT];
	uint8_t data[CONFIG_HALO_BLE_ANCS_RESPONSE_BUF_SIZE];
	uint16_t data_len;
};

/* One buffered Get App Attributes result */
struct app_result_entry {
	int status;
	char app_id[ANCS_APP_ID_BUF_LEN];
	bool has_display_name;
	uint16_t display_name_len;
	char display_name[CONFIG_HALO_BLE_ANCS_APP_NAME_MAX_LEN];
};

/* One buffered Perform Notification Action result */
struct action_result_entry {
	uint32_t uid;
	uint8_t action_id;
	int status;
};

static struct {
	/* Lua registry references (Lua thread only) */
	int notification_ref;
	int attributes_ref;
	int app_attributes_ref;
	int action_ref;
	int availability_ref;

	/* Event queues (BLE thread produces, Lua thread consumes) */
	struct k_spinlock lock;

	struct halo_ancs_notification notif_q[ANCS_NOTIF_QUEUE_LEN];
	uint8_t notif_head, notif_count;
	uint32_t notif_dropped;

	struct attr_result_entry attr_q[ANCS_RESULT_QUEUE_LEN];
	uint8_t attr_head, attr_count;

	struct app_result_entry app_q[ANCS_RESULT_QUEUE_LEN];
	uint8_t app_head, app_count;

	struct action_result_entry action_q[ANCS_ACTION_QUEUE_LEN];
	uint8_t action_head, action_count;

	bool availability_pending;
	bool availability_value;
} state = {
	.notification_ref = LUA_NOREF,
	.attributes_ref = LUA_NOREF,
	.app_attributes_ref = LUA_NOREF,
	.action_ref = LUA_NOREF,
	.availability_ref = LUA_NOREF,
};

/* ---------------------------------------------------------------------- */
/* Lua-side helpers                                                        */
/* ---------------------------------------------------------------------- */

static const char *const event_names[] = {"added", "modified", "removed"};

static const char *const category_names[] = {
	"other",          "incoming_call",      "missed_call",
	"voicemail",      "social",             "schedule",
	"email",          "news",               "health_and_fitness",
	"business_and_finance", "location",     "entertainment",
};

static const char *error_to_string(int status)
{
	switch (status) {
	case HALO_ANCS_ERR_UNKNOWN_COMMAND:
		return "unknown_command";
	case HALO_ANCS_ERR_INVALID_COMMAND:
		return "invalid_command";
	case HALO_ANCS_ERR_INVALID_PARAMETER:
		return "invalid_parameter";
	case HALO_ANCS_ERR_ACTION_FAILED:
		return "action_failed";
	case -ETIMEDOUT:
		return "timeout";
	case -ENOTCONN:
		return "disconnected";
	case -EMSGSIZE:
		return "too_large";
	case -EACCES:
		return "not_encrypted";
	case -ENOMEM:
		return "no_memory";
	default:
		return "error";
	}
}

static const char *attr_field_name(uint8_t id)
{
	switch (id) {
	case HALO_ANCS_ATTR_APP_IDENTIFIER:
		return "app_identifier";
	case HALO_ANCS_ATTR_TITLE:
		return "title";
	case HALO_ANCS_ATTR_SUBTITLE:
		return "subtitle";
	case HALO_ANCS_ATTR_MESSAGE:
		return "message";
	case HALO_ANCS_ATTR_MESSAGE_SIZE:
		return "message_size";
	case HALO_ANCS_ATTR_DATE:
		return "date";
	case HALO_ANCS_ATTR_POSITIVE_ACTION_LABEL:
		return "positive_action_label";
	case HALO_ANCS_ATTR_NEGATIVE_ACTION_LABEL:
		return "negative_action_label";
	default:
		return NULL;
	}
}

/* Run one registered Lua callback with the table on top of the stack */
static void call_lua_callback(lua_State *L, int ref, int nargs)
{
	if (ref == LUA_NOREF) {
		lua_pop(L, nargs);
		return;
	}

	lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
	lua_insert(L, -(nargs + 1)); /* move function below its arguments */

	if (lua_pcall(L, nargs, 0, 0) != LUA_OK) {
		const char *error = lua_tostring(L, -1);

		LOG_ERR("ANCS callback error: %s", error);
		lua_pop(L, 1);
	}
}

/* Push a notification event table */
static void push_notification_table(lua_State *L, const struct halo_ancs_notification *n,
				    uint32_t dropped)
{
	lua_createtable(L, 0, 10);

	if (n->event_id < ARRAY_SIZE(event_names)) {
		lua_pushstring(L, event_names[n->event_id]);
	} else {
		lua_pushstring(L, "unknown");
	}
	lua_setfield(L, -2, "event");

	lua_pushinteger(L, n->uid);
	lua_setfield(L, -2, "uid");

	if (n->category_id < ARRAY_SIZE(category_names)) {
		lua_pushstring(L, category_names[n->category_id]);
	} else {
		lua_pushstring(L, "unknown");
	}
	lua_setfield(L, -2, "category");

	lua_pushinteger(L, n->category_id);
	lua_setfield(L, -2, "category_id");

	lua_pushinteger(L, n->category_count);
	lua_setfield(L, -2, "category_count");

	lua_pushboolean(L, (n->event_flags & HALO_ANCS_FLAG_SILENT) != 0);
	lua_setfield(L, -2, "silent");
	lua_pushboolean(L, (n->event_flags & HALO_ANCS_FLAG_IMPORTANT) != 0);
	lua_setfield(L, -2, "important");
	lua_pushboolean(L, (n->event_flags & HALO_ANCS_FLAG_PRE_EXISTING) != 0);
	lua_setfield(L, -2, "pre_existing");
	lua_pushboolean(L, (n->event_flags & HALO_ANCS_FLAG_POSITIVE_ACTION) != 0);
	lua_setfield(L, -2, "positive_action");
	lua_pushboolean(L, (n->event_flags & HALO_ANCS_FLAG_NEGATIVE_ACTION) != 0);
	lua_setfield(L, -2, "negative_action");

	if (dropped > 0) {
		lua_pushinteger(L, dropped);
		lua_setfield(L, -2, "dropped");
	}
}

/* Push an attribute-result table */
static void push_attr_result_table(lua_State *L, const struct attr_result_entry *e)
{
	lua_createtable(L, 0, e->count + 2);

	lua_pushinteger(L, e->uid);
	lua_setfield(L, -2, "uid");

	if (e->status != 0) {
		lua_pushstring(L, error_to_string(e->status));
		lua_setfield(L, -2, "error");
		return;
	}

	for (uint8_t i = 0; i < e->count; i++) {
		const char *field = attr_field_name(e->ids[i]);

		if (!field || e->lengths[i] == 0) {
			/* Length 0 = attribute not available on the phone */
			continue;
		}

		if (e->ids[i] == HALO_ANCS_ATTR_MESSAGE_SIZE) {
			/* Numeric string, e.g. "142" */
			char tmp[12];
			uint16_t len = MIN(e->lengths[i], sizeof(tmp) - 1);

			memcpy(tmp, &e->data[e->offsets[i]], len);
			tmp[len] = '\0';
			lua_pushinteger(L, strtoul(tmp, NULL, 10));
		} else {
			lua_pushlstring(L, (const char *)&e->data[e->offsets[i]],
					e->lengths[i]);
		}
		lua_setfield(L, -2, field);
	}
}

/* Push an app-attribute-result table */
static void push_app_result_table(lua_State *L, const struct app_result_entry *e)
{
	lua_createtable(L, 0, 3);

	lua_pushstring(L, e->app_id);
	lua_setfield(L, -2, "app_identifier");

	if (e->status != 0) {
		lua_pushstring(L, error_to_string(e->status));
		lua_setfield(L, -2, "error");
		return;
	}

	if (e->has_display_name) {
		lua_pushlstring(L, e->display_name, e->display_name_len);
		lua_setfield(L, -2, "display_name");
	}
}

/* Push an action-result table */
static void push_action_result_table(lua_State *L, const struct action_result_entry *e)
{
	lua_createtable(L, 0, 3);

	lua_pushinteger(L, e->uid);
	lua_setfield(L, -2, "uid");

	lua_pushstring(L, e->action_id == HALO_ANCS_ACTION_POSITIVE ? "positive" : "negative");
	lua_setfield(L, -2, "action");

	if (e->status != 0) {
		lua_pushstring(L, error_to_string(e->status));
		lua_setfield(L, -2, "error");
	}
}

/**
 * @brief Lua hook: drain all pending ANCS events on the Lua thread
 */
static void ancs_hook_handler(lua_State *L, lua_Debug *ar)
{
	ARG_UNUSED(ar);

	lua_sethook(L, NULL, 0, 0);

	/* Drain until all queues are empty. Each iteration copies one item
	 * out under the spinlock, then calls into Lua unlocked. */
	while (true) {
		struct halo_ancs_notification notif = {0};
		struct attr_result_entry *attr = NULL;
		struct app_result_entry app = {0};
		struct action_result_entry action = {0};
		static struct attr_result_entry attr_copy; /* too big for stack */
		uint32_t dropped = 0;
		bool have_notif = false, have_attr = false, have_app = false;
		bool have_action = false, have_avail = false;
		bool avail_value = false;

		k_spinlock_key_t key = k_spin_lock(&state.lock);

		if (state.availability_pending) {
			state.availability_pending = false;
			avail_value = state.availability_value;
			have_avail = true;
		} else if (state.notif_count > 0) {
			notif = state.notif_q[state.notif_head];
			state.notif_head = (state.notif_head + 1) % ANCS_NOTIF_QUEUE_LEN;
			state.notif_count--;
			dropped = state.notif_dropped;
			state.notif_dropped = 0;
			have_notif = true;
		} else if (state.attr_count > 0) {
			attr_copy = state.attr_q[state.attr_head];
			attr = &attr_copy;
			state.attr_head = (state.attr_head + 1) % ANCS_RESULT_QUEUE_LEN;
			state.attr_count--;
			have_attr = true;
		} else if (state.app_count > 0) {
			app = state.app_q[state.app_head];
			state.app_head = (state.app_head + 1) % ANCS_RESULT_QUEUE_LEN;
			state.app_count--;
			have_app = true;
		} else if (state.action_count > 0) {
			action = state.action_q[state.action_head];
			state.action_head = (state.action_head + 1) % ANCS_ACTION_QUEUE_LEN;
			state.action_count--;
			have_action = true;
		}

		k_spin_unlock(&state.lock, key);

		if (have_avail) {
			lua_pushboolean(L, avail_value);
			call_lua_callback(L, state.availability_ref, 1);
		} else if (have_notif) {
			push_notification_table(L, &notif, dropped);
			call_lua_callback(L, state.notification_ref, 1);
		} else if (have_attr) {
			push_attr_result_table(L, attr);
			call_lua_callback(L, state.attributes_ref, 1);
		} else if (have_app) {
			push_app_result_table(L, &app);
			call_lua_callback(L, state.app_attributes_ref, 1);
		} else if (have_action) {
			push_action_result_table(L, &action);
			call_lua_callback(L, state.action_ref, 1);
		} else {
			break;
		}
	}
}

/* Arm the Lua hook so pending events are delivered on the Lua thread */
static void ancs_arm_hook(void)
{
	lua_State *L = halo_lua_get_state();

	if (!L || !halo_lua_is_running()) {
		return;
	}

	lua_sethook(L, ancs_hook_handler, LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE | LUA_MASKCOUNT,
		    1);
}

/* ---------------------------------------------------------------------- */
/* BLE-thread callbacks (producers)                                        */
/* ---------------------------------------------------------------------- */

static void on_ancs_availability(bool available, void *user_data)
{
	ARG_UNUSED(user_data);

	k_spinlock_key_t key = k_spin_lock(&state.lock);

	state.availability_pending = true;
	state.availability_value = available;
	k_spin_unlock(&state.lock, key);

	ancs_arm_hook();
}

static void on_ancs_notification(const struct halo_ancs_notification *notif, void *user_data)
{
	ARG_UNUSED(user_data);

	k_spinlock_key_t key = k_spin_lock(&state.lock);

	if (state.notif_count == ANCS_NOTIF_QUEUE_LEN) {
		/* Overflow: drop the oldest so the newest state wins */
		state.notif_head = (state.notif_head + 1) % ANCS_NOTIF_QUEUE_LEN;
		state.notif_count--;
		state.notif_dropped++;
	}
	state.notif_q[(state.notif_head + state.notif_count) % ANCS_NOTIF_QUEUE_LEN] = *notif;
	state.notif_count++;
	k_spin_unlock(&state.lock, key);

	ancs_arm_hook();
}

static void on_ancs_attributes(const struct halo_ancs_attr_result *result, void *user_data)
{
	ARG_UNUSED(user_data);

	k_spinlock_key_t key = k_spin_lock(&state.lock);

	if (state.attr_count == ANCS_RESULT_QUEUE_LEN) {
		LOG_WRN("ANCS attribute result queue full, dropping oldest");
		state.attr_head = (state.attr_head + 1) % ANCS_RESULT_QUEUE_LEN;
		state.attr_count--;
	}

	struct attr_result_entry *e =
		&state.attr_q[(state.attr_head + state.attr_count) % ANCS_RESULT_QUEUE_LEN];

	e->uid = result->uid;
	e->status = result->status;
	e->count = 0;
	e->data_len = 0;

	for (uint8_t i = 0; i < result->attr_count; i++) {
		const struct halo_ancs_attr *a = &result->attrs[i];
		uint16_t len = a->length;

		if (e->data_len + len > sizeof(e->data)) {
			len = sizeof(e->data) - e->data_len;
		}
		e->ids[e->count] = a->id;
		e->offsets[e->count] = e->data_len;
		e->lengths[e->count] = len;
		if (len > 0) {
			memcpy(&e->data[e->data_len], a->data, len);
			e->data_len += len;
		}
		e->count++;
	}
	state.attr_count++;
	k_spin_unlock(&state.lock, key);

	ancs_arm_hook();
}

static void on_ancs_app_attributes(const struct halo_ancs_app_attr_result *result, void *user_data)
{
	ARG_UNUSED(user_data);

	k_spinlock_key_t key = k_spin_lock(&state.lock);

	if (state.app_count == ANCS_RESULT_QUEUE_LEN) {
		LOG_WRN("ANCS app attribute result queue full, dropping oldest");
		state.app_head = (state.app_head + 1) % ANCS_RESULT_QUEUE_LEN;
		state.app_count--;
	}

	struct app_result_entry *e =
		&state.app_q[(state.app_head + state.app_count) % ANCS_RESULT_QUEUE_LEN];

	e->status = result->status;
	strncpy(e->app_id, result->app_id, sizeof(e->app_id) - 1);
	e->app_id[sizeof(e->app_id) - 1] = '\0';
	e->has_display_name = false;
	e->display_name_len = 0;

	for (uint8_t i = 0; i < result->attr_count; i++) {
		const struct halo_ancs_attr *a = &result->attrs[i];

		if (a->id == HALO_ANCS_APP_ATTR_DISPLAY_NAME && a->length > 0) {
			uint16_t len = MIN(a->length, sizeof(e->display_name));

			memcpy(e->display_name, a->data, len);
			e->display_name_len = len;
			e->has_display_name = true;
		}
	}
	state.app_count++;
	k_spin_unlock(&state.lock, key);

	ancs_arm_hook();
}

static void on_ancs_action(const struct halo_ancs_action_result *result, void *user_data)
{
	ARG_UNUSED(user_data);

	k_spinlock_key_t key = k_spin_lock(&state.lock);

	if (state.action_count == ANCS_ACTION_QUEUE_LEN) {
		state.action_head = (state.action_head + 1) % ANCS_ACTION_QUEUE_LEN;
		state.action_count--;
	}
	state.action_q[(state.action_head + state.action_count) % ANCS_ACTION_QUEUE_LEN] =
		(struct action_result_entry){
			.uid = result->uid,
			.action_id = result->action_id,
			.status = result->status,
		};
	state.action_count++;
	k_spin_unlock(&state.lock, key);

	ancs_arm_hook();
}

static const struct halo_ancs_callbacks ancs_callbacks = {
	.availability = on_ancs_availability,
	.notification = on_ancs_notification,
	.attributes = on_ancs_attributes,
	.app_attributes = on_ancs_app_attributes,
	.action = on_ancs_action,
	.user_data = NULL,
};

/* ---------------------------------------------------------------------- */
/* Lua API                                                                 */
/* ---------------------------------------------------------------------- */

static void clear_queues(void)
{
	k_spinlock_key_t key = k_spin_lock(&state.lock);

	state.notif_head = state.notif_count = 0;
	state.notif_dropped = 0;
	state.attr_head = state.attr_count = 0;
	state.app_head = state.app_count = 0;
	state.action_head = state.action_count = 0;
	state.availability_pending = false;
	k_spin_unlock(&state.lock, key);
}

/* Common register/clear helper for callback-setting functions */
static int set_callback_ref(lua_State *L, int *ref)
{
	if (lua_isnil(L, 1)) {
		if (*ref != LUA_NOREF) {
			luaL_unref(L, LUA_REGISTRYINDEX, *ref);
			*ref = LUA_NOREF;
		}
		return 0;
	}

	if (!lua_isfunction(L, 1)) {
		return luaL_error(L, "expected function or nil");
	}

	if (*ref != LUA_NOREF) {
		luaL_unref(L, LUA_REGISTRYINDEX, *ref);
	}
	lua_pushvalue(L, 1);
	*ref = luaL_ref(L, LUA_REGISTRYINDEX);
	return 0;
}

/**
 * @brief Lua: frame.ancs.notification_callback(func)
 *
 * Registering a function subscribes to iOS notifications (iOS replays all
 * existing notifications with pre_existing=true). Passing nil unsubscribes.
 */
static int lua_ancs_notification_callback(lua_State *L)
{
	int ret = set_callback_ref(L, &state.notification_ref);

	if (ret != 0) {
		return ret;
	}

	halo_ble_ancs_set_enabled(state.notification_ref != LUA_NOREF);
	return 0;
}

/**
 * @brief Lua: frame.ancs.attributes_callback(func)
 */
static int lua_ancs_attributes_callback(lua_State *L)
{
	return set_callback_ref(L, &state.attributes_ref);
}

/**
 * @brief Lua: frame.ancs.app_attributes_callback(func)
 */
static int lua_ancs_app_attributes_callback(lua_State *L)
{
	return set_callback_ref(L, &state.app_attributes_ref);
}

/**
 * @brief Lua: frame.ancs.action_callback(func)
 */
static int lua_ancs_action_callback(lua_State *L)
{
	return set_callback_ref(L, &state.action_ref);
}

/**
 * @brief Lua: frame.ancs.availability_callback(func)
 *
 * The callback is invoked once with the current availability right after
 * registration (the BLE-side callback only reports transitions, and ANCS
 * may already be available by the time a script registers), then on every
 * change.
 */
static int lua_ancs_availability_callback(lua_State *L)
{
	int ret = set_callback_ref(L, &state.availability_ref);

	if (ret == 0 && state.availability_ref != LUA_NOREF) {
		k_spinlock_key_t key = k_spin_lock(&state.lock);

		state.availability_pending = true;
		state.availability_value = halo_ble_ancs_is_available();
		k_spin_unlock(&state.lock, key);

		ancs_arm_hook();
	}

	return ret;
}

/**
 * @brief Lua: frame.ancs.is_available()
 */
static int lua_ancs_is_available(lua_State *L)
{
	lua_pushboolean(L, halo_ble_ancs_is_available());
	return 1;
}

/**
 * @brief Lua: frame.ancs.is_subscribed()
 */
static int lua_ancs_is_subscribed(lua_State *L)
{
	lua_pushboolean(L, halo_ble_ancs_is_subscribed());
	return 1;
}

/* Read an options-table entry that is either boolean (fixed attribute) or
 * a number (length-limited attribute; 'true' selects the default length) */
static void parse_attr_option(lua_State *L, int table_idx, const char *name, uint8_t id,
			      uint16_t default_max, struct halo_ancs_attr_request *req)
{
	lua_getfield(L, table_idx, name);

	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return;
	}

	uint16_t max = default_max;

	if (lua_isnumber(L, -1)) {
		lua_Integer v = lua_tointeger(L, -1);

		max = (v <= 0) ? 0 : (v > UINT16_MAX ? UINT16_MAX : (uint16_t)v);
	} else if (!lua_toboolean(L, -1)) {
		lua_pop(L, 1);
		return; /* false: attribute excluded */
	}

	lua_pop(L, 1);

	if (max == 0 && (id == HALO_ANCS_ATTR_TITLE || id == HALO_ANCS_ATTR_SUBTITLE ||
			 id == HALO_ANCS_ATTR_MESSAGE)) {
		return;
	}

	req->attr_mask |= BIT(id);
	if (id == HALO_ANCS_ATTR_TITLE) {
		req->title_max = max;
	} else if (id == HALO_ANCS_ATTR_SUBTITLE) {
		req->subtitle_max = max;
	} else if (id == HALO_ANCS_ATTR_MESSAGE) {
		req->message_max = max;
	}
}

/**
 * @brief Lua: frame.ancs.get_notification_attributes(uid, [options])
 *
 * options: {app_identifier=bool, title=n|bool, subtitle=n|bool,
 *           message=n|bool, message_size=bool, date=bool,
 *           positive_action_label=bool, negative_action_label=bool}
 *
 * Result is delivered to the attributes_callback.
 */
static int lua_ancs_get_notification_attributes(lua_State *L)
{
	uint32_t uid = (uint32_t)luaL_checkinteger(L, 1);
	struct halo_ancs_attr_request req = {0};

	if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
		luaL_checktype(L, 2, LUA_TTABLE);

		parse_attr_option(L, 2, "app_identifier", HALO_ANCS_ATTR_APP_IDENTIFIER, 0, &req);
		parse_attr_option(L, 2, "title", HALO_ANCS_ATTR_TITLE, ANCS_DEFAULT_TITLE_MAX,
				  &req);
		parse_attr_option(L, 2, "subtitle", HALO_ANCS_ATTR_SUBTITLE,
				  ANCS_DEFAULT_TITLE_MAX, &req);
		parse_attr_option(L, 2, "message", HALO_ANCS_ATTR_MESSAGE,
				  ANCS_DEFAULT_MESSAGE_MAX, &req);
		parse_attr_option(L, 2, "message_size", HALO_ANCS_ATTR_MESSAGE_SIZE, 0, &req);
		parse_attr_option(L, 2, "date", HALO_ANCS_ATTR_DATE, 0, &req);
		parse_attr_option(L, 2, "positive_action_label",
				  HALO_ANCS_ATTR_POSITIVE_ACTION_LABEL, 0, &req);
		parse_attr_option(L, 2, "negative_action_label",
				  HALO_ANCS_ATTR_NEGATIVE_ACTION_LABEL, 0, &req);

		if (req.attr_mask == 0) {
			return luaL_error(L, "no attributes requested");
		}
	} else {
		/* Default: app identifier, title, message, date */
		req.attr_mask = BIT(HALO_ANCS_ATTR_APP_IDENTIFIER) | BIT(HALO_ANCS_ATTR_TITLE) |
				BIT(HALO_ANCS_ATTR_MESSAGE) | BIT(HALO_ANCS_ATTR_DATE);
		req.title_max = ANCS_DEFAULT_TITLE_MAX;
		req.message_max = ANCS_DEFAULT_MESSAGE_MAX;
	}

	int ret = halo_ble_ancs_get_notification_attrs(uid, &req);

	if (ret == 0) {
		lua_pushboolean(L, true);
		return 1;
	}

	lua_pushboolean(L, false);
	lua_pushstring(L, ret == -EBUSY      ? "busy"
			  : ret == -ENOTCONN ? "unavailable"
					     : "error");
	return 2;
}

/**
 * @brief Lua: frame.ancs.get_app_attributes(app_identifier)
 *
 * Result is delivered to the app_attributes_callback.
 */
static int lua_ancs_get_app_attributes(lua_State *L)
{
	const char *app_id = luaL_checkstring(L, 1);

	int ret = halo_ble_ancs_get_app_attrs(app_id);

	if (ret == 0) {
		lua_pushboolean(L, true);
		return 1;
	}

	lua_pushboolean(L, false);
	lua_pushstring(L, ret == -EBUSY      ? "busy"
			  : ret == -ENOTCONN ? "unavailable"
			  : ret == -EINVAL   ? "invalid_app_identifier"
					     : "error");
	return 2;
}

/**
 * @brief Lua: frame.ancs.perform_action(uid, action)
 *
 * action: "positive" (e.g. answer call) or "negative" (e.g. dismiss).
 * Result is delivered to the action_callback (if registered).
 */
static int lua_ancs_perform_action(lua_State *L)
{
	uint32_t uid = (uint32_t)luaL_checkinteger(L, 1);
	const char *action = luaL_checkstring(L, 2);
	uint8_t action_id;

	if (strcmp(action, "positive") == 0) {
		action_id = HALO_ANCS_ACTION_POSITIVE;
	} else if (strcmp(action, "negative") == 0) {
		action_id = HALO_ANCS_ACTION_NEGATIVE;
	} else {
		return luaL_error(L, "action must be \"positive\" or \"negative\"");
	}

	int ret = halo_ble_ancs_perform_action(uid, action_id);

	if (ret == 0) {
		lua_pushboolean(L, true);
		return 1;
	}

	lua_pushboolean(L, false);
	lua_pushstring(L, ret == -EBUSY      ? "busy"
			  : ret == -ENOTCONN ? "unavailable"
					     : "error");
	return 2;
}

/* ---------------------------------------------------------------------- */
/* Service lifecycle                                                       */
/* ---------------------------------------------------------------------- */

static int ancs_service_event_handler(halo_lua_event_t event, void *user_data)
{
	ARG_UNUSED(user_data);

	switch (event) {
	case HALO_LUA_EVENT_INIT:
		state.notification_ref = LUA_NOREF;
		state.attributes_ref = LUA_NOREF;
		state.app_attributes_ref = LUA_NOREF;
		state.action_ref = LUA_NOREF;
		state.availability_ref = LUA_NOREF;
		clear_queues();
		break;

	case HALO_LUA_EVENT_DEINIT:
	case HALO_LUA_EVENT_INTERRUPT:
		/* Script is gone: unsubscribe so iOS stops streaming events
		 * nobody consumes. References are reclaimed by the Lua GC. */
		state.notification_ref = LUA_NOREF;
		state.attributes_ref = LUA_NOREF;
		state.app_attributes_ref = LUA_NOREF;
		state.action_ref = LUA_NOREF;
		state.availability_ref = LUA_NOREF;
		clear_queues();
		halo_ble_ancs_set_enabled(false);
		break;

	default:
		break;
	}

	return 0;
}

/* Always-on service: no suspend/resume handling needed (BLE manager owns
 * the ANCS client lifecycle across sleep). */
HALO_LUA_SERVICE_DEFINE(ancs_service, ancs_service_event_handler, NULL, true);

/**
 * @brief Open and register the ANCS library with the Lua VM
 */
int lua_open_ancs_library(lua_State *L)
{
	int ret = halo_lua_service_register(&ancs_service);

	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("Failed to register ANCS Lua service: %d", ret);
		return ret;
	}

	/* Hook the BLE ANCS client callbacks (idempotent) */
	halo_ble_ancs_set_callbacks(&ancs_callbacks);

	/* Get or create the global 'frame' table */
	lua_getglobal(L, "frame");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setglobal(L, "frame");
	}

	/* Create 'ancs' subtable */
	lua_newtable(L);

	lua_pushcfunction(L, lua_ancs_notification_callback);
	lua_setfield(L, -2, "notification_callback");

	lua_pushcfunction(L, lua_ancs_attributes_callback);
	lua_setfield(L, -2, "attributes_callback");

	lua_pushcfunction(L, lua_ancs_app_attributes_callback);
	lua_setfield(L, -2, "app_attributes_callback");

	lua_pushcfunction(L, lua_ancs_action_callback);
	lua_setfield(L, -2, "action_callback");

	lua_pushcfunction(L, lua_ancs_availability_callback);
	lua_setfield(L, -2, "availability_callback");

	lua_pushcfunction(L, lua_ancs_is_available);
	lua_setfield(L, -2, "is_available");

	lua_pushcfunction(L, lua_ancs_is_subscribed);
	lua_setfield(L, -2, "is_subscribed");

	lua_pushcfunction(L, lua_ancs_get_notification_attributes);
	lua_setfield(L, -2, "get_notification_attributes");

	lua_pushcfunction(L, lua_ancs_get_app_attributes);
	lua_setfield(L, -2, "get_app_attributes");

	lua_pushcfunction(L, lua_ancs_perform_action);
	lua_setfield(L, -2, "perform_action");

	/* Set 'ancs' table in 'frame' */
	lua_setfield(L, -2, "ancs");

	/* Pop frame table */
	lua_pop(L, 1);

	LOG_DBG("ANCS library registered");
	return 0;
}
