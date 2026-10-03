#pragma once
typedef long atomic_t;
typedef long atomic_val_t;
#define ATOMIC_INIT(x) (x)
#define atomic_get(p) (*(p))
#define atomic_set(p, v) (*(p) = (v))
#include <stdbool.h>
static inline bool atomic_cas(atomic_t *p, atomic_val_t old, atomic_val_t nv)
{
	if (*p != old) {
		return false;
	}
	*p = nv;
	return true;
}
static inline atomic_val_t atomic_inc(atomic_t *p)
{
	return (*p)++;
}
