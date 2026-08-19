#pragma once
typedef long atomic_t;
typedef long atomic_val_t;
#define ATOMIC_INIT(x) (x)
#define atomic_get(p) (*(p))
#define atomic_set(p, v) (*(p) = (v))
