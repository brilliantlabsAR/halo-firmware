#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
/* virtual clock, advanced by the test harness (20ms per block) */
extern uint32_t host_uptime_ms;
static inline uint32_t k_uptime_get_32(void) { return host_uptime_ms; }
static inline int64_t k_uptime_get(void) { return (int64_t)host_uptime_ms; }
/* pretend 1 cycle = 1us so cyc-to-us conversions are identity */
static inline uint32_t k_cycle_get_32(void) { return host_uptime_ms * 1000u; }
static inline uint32_t k_cyc_to_us_floor32(uint32_t c) { return c; }
static inline uint64_t k_cyc_to_us_floor64(uint32_t c) { return c; }
