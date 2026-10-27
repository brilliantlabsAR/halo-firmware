/*
 * See include/drivers/i2c_glitch_guard.h.
 */

#include <zephyr/kernel.h>
#include <drivers/i2c_glitch_guard.h>

/* One shared bus on this board; the k_mutex is recursive for its owner. */
static K_MUTEX_DEFINE(i2c_glitch_guard_mutex);

void i2c_glitch_guard_lock(void)
{
	(void)k_mutex_lock(&i2c_glitch_guard_mutex, K_FOREVER);
}

void i2c_glitch_guard_unlock(void)
{
	(void)k_mutex_unlock(&i2c_glitch_guard_mutex);
}
