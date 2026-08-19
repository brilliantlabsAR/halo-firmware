/*
 * Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "lua.h"
#include "lauxlib.h"
#include <halo/lua_time.h>
#include <halo/lua_service.h>

LOG_MODULE_REGISTER(lua_time, CONFIG_HALO_LOG_LEVEL);

/**
 * @brief Time state structure
 * 
 * Tracks UTC time and time zone information.
 * UTC time is synchronized with uptime to maintain accuracy.
 * If not explicitly set, UTC time equals uptime (system boot time = epoch 0).
 */
static struct {
	uint32_t sync_uptime_ms;      /* Uptime when UTC was last set */
	uint64_t utc_time_ms;         /* UTC time in milliseconds */
	/* Signed total offset in minutes (-720 = UTC-12:00 .. +840 = UTC+14:00).
	 * Kept as one signed quantity rather than separate hour/minute fields:
	 * splitting them invites applying the sign to the hours only, which made
	 * every negative half-hour zone wrong by twice its minutes.
	 */
	int16_t tz_offset_minutes;
} time_state = {
	.sync_uptime_ms = 0,
	.utc_time_ms = 0,
	.tz_offset_minutes = 0,
};

/**
 * @brief Get current UTC time in milliseconds
 * 
 * Calculates current UTC by adding elapsed uptime to last sync point.
 * If UTC was never set explicitly, returns uptime (boot time = epoch 0).
 * 
 * @return Current UTC time in milliseconds
 */
static uint64_t get_current_utc_ms(void)
{
	uint32_t current_uptime = k_uptime_get_32();
	uint32_t elapsed_ms = current_uptime - time_state.sync_uptime_ms;
	return time_state.utc_time_ms + elapsed_ms;
}

/**
 * @brief Lua: frame.time.utc([timestamp])
 * 
 * Get or set UTC time.
 * Without arguments: returns current UTC timestamp in seconds
 *   - If never set: returns uptime in seconds (boot time = epoch 0)
 *   - If previously set: returns synchronized UTC time
 * With argument: sets UTC time from Unix timestamp (seconds)
 * 
 * @param L Lua state
 * @return 1 (UTC timestamp in seconds)
 */
static int lua_time_utc(lua_State *L)
{
	int nargs = lua_gettop(L);
	
	if (nargs == 0) {
		/* Get current UTC time (or uptime if never set) */
		uint64_t utc_ms = get_current_utc_ms();
		lua_Number utc_seconds = (lua_Number)utc_ms / 1000.0;
		lua_pushnumber(L, utc_seconds);
		return 1;
	}
	
	/* Set UTC time */
	lua_Number timestamp = luaL_checknumber(L, 1);
	if (timestamp < 0) {
		return luaL_error(L, "timestamp must be non-negative");
	}
	
	time_state.sync_uptime_ms = k_uptime_get_32();
	time_state.utc_time_ms = (uint64_t)(timestamp * 1000.0);
	
	LOG_DBG("Time UTC set: %.3f", timestamp);
	/* Return the set value */
	lua_pushnumber(L, timestamp);
	return 1;
}

/**
 * @brief Format the stored offset as "+HH:MM" / "-HH:MM"
 *
 * The sign comes from the signed total, so it stays correct for offsets whose
 * hour component is zero.
 *
 * @return Pointer to a static buffer holding the formatted offset
 */
static const char *format_tz_offset(void)
{
	static char tz_string[10];
	int total = time_state.tz_offset_minutes;
	int magnitude = abs(total);

	snprintf(tz_string, sizeof(tz_string), "%c%02d:%02d",
	         total < 0 ? '-' : '+', magnitude / 60, magnitude % 60);

	return tz_string;
}

/**
 * @brief Lua: frame.time.zone([offset])
 *
 * Get or set time zone offset.
 * Without arguments: returns current time zone as string (e.g., "+08:00")
 * With argument: sets time zone from string (e.g., "+08:00", "-05:30") and
 * returns the stored zone in the same format.
 *
 * @param L Lua state
 * @return 1 (time zone string)
 */
static int lua_time_zone(lua_State *L)
{
	int nargs = lua_gettop(L);
	
	if (nargs == 0) {
		/* Get current time zone */
		lua_pushstring(L, format_tz_offset());
		return 1;
	}

	/* Set time zone */
	const char *tz_str = luaL_checkstring(L, 1);
	unsigned int hour = 0;
	unsigned int minute = 0;
	int sign = 1;
	const char *p = tz_str;

	/* Take the sign from the string rather than from the parsed hour: a
	 * signed conversion loses it entirely for "-00:30", and attaching it to
	 * the hour alone mis-signs the minutes.
	 */
	if (*p == '-') {
		sign = -1;
		p++;
	} else if (*p == '+') {
		p++;
	}

	/* Parse time zone string (e.g., "+08:00", "-05:30") */
	if (sscanf(p, "%u:%u", &hour, &minute) != 2) {
		return luaL_error(L, "time zone must be in format '+HH:MM' or '-HH:MM'");
	}

	/* Validate hour range (-12 to +14) */
	if ((sign > 0 && hour > 14) || (sign < 0 && hour > 12)) {
		return luaL_error(L, "hour must be between -12 and +14");
	}

	/* Validate minute values (0, 30, 45) */
	if (minute != 0 && minute != 30 && minute != 45) {
		return luaL_error(L, "minute must be 0, 30, or 45");
	}

	/* Special case: UTC-12 and UTC+14 must have 0 minutes */
	if (hour == (sign > 0 ? 14 : 12) && minute != 0) {
		return luaL_error(L, "UTC-12 and UTC+14 must have 0 minutes");
	}

	/* Update time zone: one signed total, so the sign cannot be lost */
	time_state.tz_offset_minutes = (int16_t)(sign * (int)(hour * 60 + minute));

	/* Echo the stored zone back, as PROTOCOL.md documents and as
	 * frame.time.utc() does for its own setter.
	 */
	lua_pushstring(L, format_tz_offset());
	return 1;
}

/**
 * @brief Convert Unix timestamp to Lua date table
 * 
 * Creates a Lua table with date/time components:
 * - second: 0-59
 * - minute: 0-59
 * - hour: 0-23
 * - day: 1-31
 * - month: 1-12
 * - year: full year (e.g., 2025)
 * - weekday: 0-6 (0=Sunday)
 * - 'day of year': 0-365
 * - 'is daylight saving': daylight saving time flag
 *
 * @param L Lua state
 * @param timestamp Unix timestamp in seconds
 */
static void table_from_timestamp(lua_State *L, time_t timestamp)
{
	struct tm time_parts;
	
	/* Convert to broken-down time (UTC) */
	gmtime_r(&timestamp, &time_parts);
	
	/* Create Lua table */
	lua_newtable(L);
	
	lua_pushinteger(L, time_parts.tm_sec);
	lua_setfield(L, -2, "second");
	
	lua_pushinteger(L, time_parts.tm_min);
	lua_setfield(L, -2, "minute");
	
	lua_pushinteger(L, time_parts.tm_hour);
	lua_setfield(L, -2, "hour");
	
	lua_pushinteger(L, time_parts.tm_mday);
	lua_setfield(L, -2, "day");
	
	lua_pushinteger(L, time_parts.tm_mon + 1);  /* Lua uses 1-12 */
	lua_setfield(L, -2, "month");
	
	lua_pushinteger(L, time_parts.tm_year + 1900);
	lua_setfield(L, -2, "year");
	
	lua_pushinteger(L, time_parts.tm_wday);  /* 0-6, 0=Sunday */
	lua_setfield(L, -2, "weekday");
	
	lua_pushinteger(L, time_parts.tm_yday);  /* 0-365 */
	lua_setfield(L, -2, "day of year");
	
	lua_pushboolean(L, time_parts.tm_isdst);
	lua_setfield(L, -2, "is daylight saving");
}

/**
 * @brief Lua: frame.time.date([timestamp])
 * 
 * Get local date/time as a table.
 * Without arguments: returns current local time (or uptime + timezone if UTC never set)
 * With argument: converts Unix timestamp to local time
 * 
 * @param L Lua state
 * @return 1 (date table)
 */
static int lua_time_date(lua_State *L)
{
	int nargs = lua_gettop(L);
	time_t local_timestamp;
	
	if (nargs == 0) {
		/* Get current local time (works even if UTC was never set) */
		uint64_t utc_ms = get_current_utc_ms();
		time_t utc_seconds = (time_t)(utc_ms / 1000);
		
		/* Apply time zone offset */
		int32_t tz_offset_seconds = time_state.tz_offset_minutes * 60;
		local_timestamp = utc_seconds + tz_offset_seconds;
	} else {
		/* Convert provided timestamp to local time */
		lua_Number timestamp = luaL_checknumber(L, 1);
		if (timestamp < 0) {
			return luaL_error(L, "timestamp must be non-negative");
		}
		
		/* Apply time zone offset */
		int32_t tz_offset_seconds = time_state.tz_offset_minutes * 60;
		local_timestamp = (time_t)timestamp + tz_offset_seconds;
	}
	
	/* Convert to table */
	table_from_timestamp(L, local_timestamp);
	return 1;
}

/**
 * @brief Service lifecycle event handler
 * 
 * Handles events like INTERRUPT (Ctrl+C) to reset state.
 */
static int time_service_event_handler(halo_lua_event_t event, void *user_data)
{
	ARG_UNUSED(user_data);
	
	switch (event) {
	case HALO_LUA_EVENT_INIT:
		break;
		
	case HALO_LUA_EVENT_DEINIT:
		break;
		
	case HALO_LUA_EVENT_INTERRUPT:
		/* Note: We don't reset time state on Ctrl+C as it should persist */
		break;
	
	default:
		break;
	}
	
	return 0;
}

/* Register service with lifecycle management (always-on service) */
HALO_LUA_SERVICE_DEFINE(time_service, time_service_event_handler, NULL, true);

/**
 * @brief Open and register time library with Lua VM
 * 
 * Registers all time functions under frame.time table.
 */
int lua_open_time_library(lua_State *L)
{
	/* Register service for lifecycle management */
	int ret = halo_lua_service_register(&time_service);
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("Failed to register time service: %d", ret);
		return ret;
	}

	/* Get or create the global 'frame' table */
	lua_getglobal(L, "frame");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setglobal(L, "frame");
	}

	/* Create 'time' subtable */
	lua_newtable(L);

	/* Register time functions */
	lua_pushcfunction(L, lua_time_utc);
	lua_setfield(L, -2, "utc");

	lua_pushcfunction(L, lua_time_zone);
	lua_setfield(L, -2, "zone");

	lua_pushcfunction(L, lua_time_date);
	lua_setfield(L, -2, "date");

	/* Set 'time' table in 'frame' */
	lua_setfield(L, -2, "time");

	/* Pop frame table */
	lua_pop(L, 1);

	return 0;
}
