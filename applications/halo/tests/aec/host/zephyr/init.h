#pragma once
#define SYS_INIT(fn, level, prio) \
	int fn##_host_hook(void) { return fn(); }
#define APPLICATION 0
#ifndef CONFIG_APPLICATION_INIT_PRIORITY
#define CONFIG_APPLICATION_INIT_PRIORITY 90
#endif
