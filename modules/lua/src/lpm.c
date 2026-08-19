/*
** $Id: lpm.c $
** Power Management library for Lua
*/

#define lpm_c
#define LUA_LIB

#include "lprefix.h"

#include <stdlib.h>
#include <time.h>

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"

#include <zephyr/pm/pm.h>
#include <zephyr/pm/state.h>
#include <zephyr/kernel.h>

/*
** Set the system to sleep for a specified number of milliseconds
*/
static int lpmsleep (lua_State *L) {
  lua_Integer milliseconds = luaL_checkinteger(L, 1);
  k_msleep(milliseconds);
  return 0;
}

/*
** Force the system into a specific power state
*/
static int lpmforce (lua_State *L) {
  const char *state_name = luaL_optstring(L, 1, "suspend-to-ram");
  
  struct pm_state_info info = {0};
  
  if (strcmp(state_name, "runtime-idle") == 0) {
    info.state = PM_STATE_RUNTIME_IDLE;
  } else if (strcmp(state_name, "suspend-to-idle") == 0) {
    info.state = PM_STATE_SUSPEND_TO_IDLE;
  } else if (strcmp(state_name, "standby") == 0) {
    info.state = PM_STATE_STANDBY;
  } else if (strcmp(state_name, "suspend-to-ram") == 0) {
    info.state = PM_STATE_SUSPEND_TO_RAM;
  } else if (strcmp(state_name, "suspend-to-disk") == 0) {
    info.state = PM_STATE_SUSPEND_TO_DISK;
  } else if (strcmp(state_name, "soft-off") == 0) {
    info.state = PM_STATE_SOFT_OFF;
  } else {
    return luaL_error(L, "invalid power state: %s", state_name);
  }
  
  // Force the power state
  pm_state_force(0, &info);
  
  // Trigger sleep
  k_sleep(K_SECONDS(1));
  
  return 0;
}

static const luaL_Reg pmlib[] = {
  {"sleep", lpmsleep},
  {"force", lpmforce},
  {NULL, NULL}
};

LUAMOD_API int luaopen_pm (lua_State *L) {
  luaL_newlib(L, pmlib);
  return 1;
}