/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/pm/pm.h>
#include <zephyr/pm/policy.h>
#include <zephyr/sys/dlist.h>
#include <zephyr/sys/reboot.h>

#include "halo/pm_manager.h"

#ifdef CONFIG_HALO_SFXR
#include "halo/sfxr.h"
#endif

#ifdef CONFIG_HALO_LUA_SPEAKER
#include "halo/lua_speaker.h"
#endif

#ifdef CONFIG_HALO_WATCHDOG_MANAGER
#include "halo/watchdog_manager.h"
#endif

#include <power_mgr.h>

LOG_MODULE_REGISTER(halo_pm, CONFIG_HALO_LOG_LEVEL);

static void pm_sleep_work_handler(struct k_work *work);

#define PM_MANAGER_INIT_MAGIC 0x504D474D /* 'PMGM' */

/* Max time to wait for in-flight audio playback to drain before deep sleep. */
#define PM_DEEP_AUDIO_QUIESCE_MS 1500

/* SOFT_OFF is entered from the idle thread after the policy lock is released;
 * on success the SoC powers off and the sleeping thread never resumes. If we
 * are still executing after this long, entry was blocked and we must recover
 * instead of leaving the device dark but awake. */
#define PM_DEEP_ENTRY_GRACE_MS 2000

/* PM manager context */
static struct {
	uint32_t initialized;
	struct k_mutex lock;
	sys_dlist_t callbacks; /* List of registered callbacks */

	/* Sleep state. Atomic (not mutex-guarded): halo_pm_is_sleeping() and
	 * halo_pm_wakeup() are called from BLE/button/IMU event paths that
	 * must never block behind a thread running the suspend chain. */
	atomic_t is_sleeping;
	struct k_sem wakeup_sem;               /* Semaphore for wakeup signaling */
	struct k_sem resume_done_sem;          /* Given when a light/standby resume completes */
	halo_pm_wakeup_source_t wakeup_source; /* Last wakeup source */
	struct k_thread *sleep_thread;         /* Thread currently blocked in light sleep */

	/* Async sleep work */
	struct k_work sleep_work;
	uint32_t sleep_timeout_ms;

	/* Statistics */
	uint32_t light_sleep_count;
	uint32_t standby_count;
	uint32_t deep_sleep_count;
	halo_pm_sleep_mode_t last_mode;

	/* RTC wakeup state tracking */
	bool rtc_wakeup_enabled;
} pm_ctx __attribute__((noinit));

static void pm_sleep_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	int ret = halo_pm_sleep_standby(pm_ctx.sleep_timeout_ms);
	if (ret != 0) {
		LOG_ERR("Failed to enter standby sleep: %d", ret);
	}
}

/* Failsafe for a wedged shutdown: if the deep-sleep suspend chain blocks
 * indefinitely (e.g. a peripheral deinit waiting on an event that never
 * comes), nothing else can recover the device — the button thread is the one
 * running the chain. Reboot into a clean state instead of hanging dark. */
#define PM_SHUTDOWN_FAILSAFE_MS 15000

static void pm_shutdown_failsafe_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_ERR("Deep sleep shutdown wedged for %d ms, rebooting", PM_SHUTDOWN_FAILSAFE_MS);
	log_panic();
	k_sleep(K_MSEC(50));
	sys_reboot(SYS_REBOOT_COLD);
}

static K_WORK_DELAYABLE_DEFINE(pm_shutdown_failsafe, pm_shutdown_failsafe_handler);

/* Failure forensics: which threads are alive when SOFT_OFF cannot be entered */
static void pm_dump_thread_cb(const struct k_thread *thread, void *user_data)
{
	ARG_UNUSED(user_data);

	char state[32];
	const char *name = k_thread_name_get((k_tid_t)thread);

	LOG_ERR("  thread %-18s prio %3d state %s", name ? name : "?", thread->base.prio,
		k_thread_state_str((k_tid_t)thread, state, sizeof(state)));
}

int halo_pm_init(void)
{

	/* Initialize mutex and semaphore Every time to ensure proper state */
	pm_policy_state_lock_get(PM_STATE_SOFT_OFF, PM_ALL_SUBSTATES);

	k_mutex_init(&pm_ctx.lock);
	k_sem_init(&pm_ctx.wakeup_sem, 0, 1);
	k_sem_init(&pm_ctx.resume_done_sem, 0, 1);
	sys_dlist_init(&pm_ctx.callbacks);
	k_work_init(&pm_ctx.sleep_work, pm_sleep_work_handler);

	atomic_set(&pm_ctx.is_sleeping, 0);

	if (power_mgr_cold_boot()) {
		int ret = power_mgr_set_offprofile(PM_STATE_MODE_STOP);
		if (ret != 0) {
			LOG_ERR("Failed to set offprofile: %d", ret);
			return ret;
		}
	} else {
		uint32_t wakeup_reason = power_mgr_resolve_wakeup_reason();
		if (wakeup_reason == PM_WAKEUP_BLE) {
			pm_ctx.wakeup_source = HALO_PM_WAKEUP_BLE;
		} else if (wakeup_reason == PM_WAKEUP_RTC) {
			pm_ctx.wakeup_source = HALO_PM_WAKEUP_TIMEOUT;
		} else if (wakeup_reason == PM_WAKEUP_LPGPIO0) {
			pm_ctx.wakeup_source = HALO_PM_WAKEUP_MICROPHONE;
		} else if (wakeup_reason == PM_WAKEUP_LPGPIO1) {
			pm_ctx.wakeup_source = HALO_PM_WAKEUP_BUTTON;
		} else {
			pm_ctx.wakeup_source = HALO_PM_WAKEUP_UNKNOWN;
		}
	}

	// Don't re-initialize if already done
	if (pm_ctx.initialized != PM_MANAGER_INIT_MAGIC) {
		/* Initialize light-sleep state */
		atomic_set(&pm_ctx.is_sleeping, 0);
		pm_ctx.wakeup_source = HALO_PM_WAKEUP_UNKNOWN;

		pm_ctx.sleep_timeout_ms = 0;
		pm_ctx.sleep_thread = NULL;

		pm_ctx.light_sleep_count = 0;
		pm_ctx.standby_count = 0;
		pm_ctx.deep_sleep_count = 0;
		pm_ctx.last_mode = HALO_PM_SLEEP_NONE;
		pm_ctx.rtc_wakeup_enabled = true; /* Default: RTC wakeup enabled */

		pm_ctx.initialized = PM_MANAGER_INIT_MAGIC;
	}

	power_mgr_ready_for_sleep();

	uint32_t ispr0, ispr1;

	power_mgr_get_boot_pending(&ispr0, &ispr1);
	LOG_INF("Boot: cold=%d wakeup_raw=%u nvic_ispr0=0x%08x nvic_ispr1=0x%08x",
		power_mgr_cold_boot(), power_mgr_get_wakeup_reason(), ispr0, ispr1);

	LOG_INF("Power management initialized, wakeup source: %s",
		halo_pm_wakeup_source_name(pm_ctx.wakeup_source));

	return 0;
}

int halo_pm_register_callback(struct halo_pm_callback *cb, halo_pm_callback_t callback,
			      void *user_data, const char *name, int priority)
{
	if (!cb || !callback) {
		return -EINVAL;
	}

	if (pm_ctx.initialized != PM_MANAGER_INIT_MAGIC) {
		return -ENODEV;
	}

	cb->callback = callback;
	cb->user_data = user_data;
	cb->name = name ? name : "unnamed";
	cb->priority = priority;
	cb->suspended = false; /* Initialize suspend tracking flag */

	k_mutex_lock(&pm_ctx.lock, K_FOREVER);

	/* Insert callback in priority order (highest priority first) */
	sys_dnode_t *node;
	struct halo_pm_callback *item;
	bool inserted = false;

	SYS_DLIST_FOR_EACH_NODE(&pm_ctx.callbacks, node) {
		item = CONTAINER_OF(node, struct halo_pm_callback, node);
		if (priority > item->priority) {
			sys_dlist_insert(node, &cb->node);
			inserted = true;
			break;
		}
	}

	if (!inserted) {
		sys_dlist_append(&pm_ctx.callbacks, &cb->node);
	}

	k_mutex_unlock(&pm_ctx.lock);

	return 0;
}

int halo_pm_unregister_callback(struct halo_pm_callback *cb)
{
	if (!cb) {
		return -EINVAL;
	}

	if (pm_ctx.initialized != PM_MANAGER_INIT_MAGIC) {
		return -ENODEV;
	}

	k_mutex_lock(&pm_ctx.lock, K_FOREVER);

	sys_dlist_remove(&cb->node);

	k_mutex_unlock(&pm_ctx.lock);

	return 0;
}

/* Call all callbacks for suspend event.
 *
 * Called WITHOUT pm_ctx.lock held: suspend callbacks block for seconds (BLE
 * teardown, Lua runtime exit) and event paths running concurrently (BLE data,
 * disconnect handlers) call halo_pm_is_sleeping()/halo_pm_wakeup() — holding
 * the mutex across the chain deadlocked shutdown whenever a peer was
 * connected. Safe because the callback list is append-only after init
 * (nothing calls halo_pm_unregister_callback) and sleep entry is serialized
 * by the is_sleeping atomic. */
static int pm_notify_suspend(halo_pm_sleep_mode_t mode)
{
	sys_dnode_t *node;
	struct halo_pm_callback *cb;
	int ret;

	/* Call callbacks in priority order (highest first) */
	SYS_DLIST_FOR_EACH_NODE(&pm_ctx.callbacks, node) {
		cb = CONTAINER_OF(node, struct halo_pm_callback, node);
		LOG_DBG("PM suspend callback: %s (prio=%d)", cb->name, cb->priority);

		/* Skip if already suspended */
		if (cb->suspended) {
			continue;
		}

		ret = cb->callback(HALO_PM_EVENT_SUSPEND, mode, cb->user_data);
		if (ret < 0) {
			LOG_ERR("Suspend callback failed: %s (ret=%d)", cb->name, ret);
			cb->suspended = false; /* Mark as not suspended on error */
			return ret;
		} else if (ret == 0) {
			/* Callback suspended successfully, mark for resume */
			cb->suspended = true;
		} else {
			/* Callback skipped (ret == 1), don't mark for resume */
			cb->suspended = false;
		}
	}

	return 0;
}

/* Call all callbacks for resume event */
static int pm_notify_resume(halo_pm_sleep_mode_t mode)
{
	sys_dnode_t *node;
	sys_dnode_t *prev_node;
	struct halo_pm_callback *cb;
	int ret;

	/* Call callbacks in reverse priority order (lowest first) */
	/* Iterate from tail to head */
	node = sys_dlist_peek_tail(&pm_ctx.callbacks);
	while (node != NULL) {
		cb = CONTAINER_OF(node, struct halo_pm_callback, node);
		LOG_DBG("PM resume callback: %s (prio=%d)", cb->name, cb->priority);

		/* Get previous node before calling callback (in case it modifies list) */
		prev_node = sys_dlist_peek_prev(&pm_ctx.callbacks, node);

		/* Only resume callbacks that were actually suspended */
		if (cb->suspended) {
			ret = cb->callback(HALO_PM_EVENT_RESUME, mode, cb->user_data);
			if (ret < 0) {
				LOG_WRN("Resume callback failed: %s (ret=%d)", cb->name, ret);
				/* Continue with other callbacks even on error */
			}
			cb->suspended = false; /* Clear flag after resume */
		} else {
		}

		node = prev_node;
	}

	return 0;
}

int halo_pm_sleep_light(uint32_t timeout_ms)
{
	int ret;

	if (pm_ctx.initialized != PM_MANAGER_INIT_MAGIC) {
		return -ENODEV;
	}

	/* Early check so a rejected second caller cannot touch the handshake
	 * state of a sleeper that is already parked (the cas below re-checks). */
	if (atomic_get(&pm_ctx.is_sleeping)) {
		LOG_WRN("Already in sleep mode, ignoring light sleep request");
		return -EALREADY;
	}

	/* Arm the wakeup handshake BEFORE publishing the sleeping flag: the
	 * suspend chain below runs unlocked, so a wakeup may arrive while it
	 * is still executing — resetting the semaphore any later would erase
	 * that wakeup and sleep through it. */
	k_mutex_lock(&pm_ctx.lock, K_FOREVER);
	(void)k_sem_reset(&pm_ctx.wakeup_sem);
	pm_ctx.sleep_thread = k_current_get();
	k_mutex_unlock(&pm_ctx.lock);

	if (!atomic_cas(&pm_ctx.is_sleeping, 0, 1)) {
		LOG_WRN("Already in sleep mode, ignoring light sleep request");
		k_mutex_lock(&pm_ctx.lock, K_FOREVER);
		pm_ctx.sleep_thread = NULL;
		k_mutex_unlock(&pm_ctx.lock);
		return -EALREADY;
	}

	LOG_WRN("Entering light sleep (BLE ON) (timeout=%u ms)", timeout_ms);

	pm_ctx.last_mode = HALO_PM_SLEEP_LIGHT;

	/* Notify all callbacks about suspend (unlocked — see pm_notify_suspend) */
	ret = pm_notify_suspend(HALO_PM_SLEEP_LIGHT);
	if (ret < 0) {
		LOG_ERR("Failed to suspend (ret=%d)", ret);
		pm_notify_resume(HALO_PM_SLEEP_LIGHT);
		pm_ctx.last_mode = HALO_PM_SLEEP_NONE;
		atomic_set(&pm_ctx.is_sleeping, 0);
		k_mutex_lock(&pm_ctx.lock, K_FOREVER);
		pm_ctx.sleep_thread = NULL;
		k_mutex_unlock(&pm_ctx.lock);
		return ret;
	}

	/* Update statistics */
	pm_ctx.light_sleep_count++;

	/* Light sleep keeps the full wake set: button, mic AAD, and RTC when a
	 * timeout was requested. (The off profile is shared with deep sleep,
	 * which restricts it to button-only — re-arm everything here.) */
	ret = power_mgr_set_wakeup_sources(true, true, timeout_ms != 0);
	if (ret != 0) {
		LOG_WRN("Failed to set light sleep wakeup sources: %d", ret);
	}

	/* The SOFT_OFF policy lock stays HELD across light sleep (same as
	 * standby). Light sleep promises BLE stays up, and SOFT_OFF is the
	 * board's only Zephyr CPU state — full power-off, BLE gone, reboot
	 * through MCUboot on wake. Releasing the lock here (as this function
	 * once did) made the idle thread free to power the device off if
	 * kernel timeouts ever went quiet during the sleep. A caller who
	 * wants the device off uses halo_pm_sleep_deep(), which releases the
	 * lock itself — including when entered while a light sleeper is
	 * parked (wake it, wait on resume_done_sem, then run the deep path).
	 */

	/* Enter light sleep state */
	if (timeout_ms > 0) {
		ret = k_sem_take(&pm_ctx.wakeup_sem, K_MSEC(timeout_ms));
	} else {
		ret = k_sem_take(&pm_ctx.wakeup_sem, K_FOREVER);
	}

	k_mutex_lock(&pm_ctx.lock, K_FOREVER);
	pm_ctx.sleep_thread = NULL;
	if (atomic_cas(&pm_ctx.is_sleeping, 1, 0)) {
		/* Woken by timeout, not by halo_pm_wakeup(). */
		pm_ctx.wakeup_source = HALO_PM_WAKEUP_TIMEOUT;
	}

	LOG_INF("Woke up from light sleep (source: %s)",
		halo_pm_wakeup_source_name(pm_ctx.wakeup_source));
	k_mutex_unlock(&pm_ctx.lock);

	/* Notify all callbacks about resume (unlocked) */
	pm_notify_resume(HALO_PM_SLEEP_LIGHT);

	/* Signal anyone waiting to take over the sleep slot (deep-from-sleep) */
	k_sem_give(&pm_ctx.resume_done_sem);

	LOG_INF("Resumed from light sleep");

	return 0;
}

int halo_pm_sleep_standby(uint32_t timeout_ms)
{
	int ret;

	if (pm_ctx.initialized != PM_MANAGER_INIT_MAGIC) {
		return -ENODEV;
	}

	/* Early check so a rejected second caller cannot touch the handshake
	 * state of a sleeper that is already parked (the cas below re-checks). */
	if (atomic_get(&pm_ctx.is_sleeping)) {
		LOG_WRN("Already in sleep mode, ignoring standby request");
		return -EALREADY;
	}

	/* Arm the wakeup handshake before publishing the sleeping flag (see
	 * halo_pm_sleep_light for why the ordering matters). */
	k_mutex_lock(&pm_ctx.lock, K_FOREVER);
	(void)k_sem_reset(&pm_ctx.wakeup_sem);
	pm_ctx.wakeup_source = HALO_PM_WAKEUP_UNKNOWN;
	k_mutex_unlock(&pm_ctx.lock);

	if (!atomic_cas(&pm_ctx.is_sleeping, 0, 1)) {
		LOG_WRN("Already in sleep mode, ignoring standby request");
		return -EALREADY;
	}

	LOG_INF("Entering standby mode (timeout=%u ms)", timeout_ms);

	pm_ctx.last_mode = HALO_PM_SLEEP_STANDBY;

	/* Notify all callbacks about suspend (Lua runtime will install hook
	 * here) — unlocked, see pm_notify_suspend. */
	ret = pm_notify_suspend(HALO_PM_SLEEP_STANDBY);
	if (ret < 0) {
		LOG_ERR("Failed to suspend (ret=%d)", ret);
		pm_notify_resume(HALO_PM_SLEEP_STANDBY);
		pm_ctx.last_mode = HALO_PM_SLEEP_NONE;
		atomic_set(&pm_ctx.is_sleeping, 0);
		return ret;
	}

	/* Update statistics */
	pm_ctx.standby_count++;

	LOG_INF("System is now in standby mode");
	/* Wait for wakeup signal (standby - CPU active but peripherals off) */
	if (timeout_ms > 0) {
		ret = k_sem_take(&pm_ctx.wakeup_sem, K_MSEC(timeout_ms));
		if (ret == -EAGAIN) {
			LOG_INF("standby timeout reached");
		}
	} else {
		k_sem_take(&pm_ctx.wakeup_sem, K_FOREVER);
	}

	k_mutex_lock(&pm_ctx.lock, K_FOREVER);
	if (atomic_cas(&pm_ctx.is_sleeping, 1, 0)) {
		/* Woken by timeout, not by halo_pm_wakeup(). */
		pm_ctx.wakeup_source = HALO_PM_WAKEUP_TIMEOUT;
	}
	k_mutex_unlock(&pm_ctx.lock);

	/* Notify all callbacks about resume (Lua runtime will remove hook
	 * here) — unlocked. */
	pm_notify_resume(HALO_PM_SLEEP_STANDBY);

	/* Signal anyone waiting to take over the sleep slot (deep-from-sleep) */
	k_sem_give(&pm_ctx.resume_done_sem);

	LOG_INF("Woke up from standby");
	return 0;
}

int halo_pm_sleep_standby_async(uint32_t timeout_ms)
{
	if (pm_ctx.initialized != PM_MANAGER_INIT_MAGIC) {
		return -ENODEV;
	}

	LOG_INF("Scheduling async standby sleep (timeout=%u ms)", timeout_ms);

	pm_ctx.sleep_timeout_ms = timeout_ms;
	k_work_submit(&pm_ctx.sleep_work);

	return 0;
}

int halo_pm_sleep_deep(uint32_t timeout_ms)
{
	int ret;

	if (pm_ctx.initialized != PM_MANAGER_INIT_MAGIC) {
		return -ENODEV;
	}

	if (!atomic_cas(&pm_ctx.is_sleeping, 0, 1)) {
		/* Device is in light/standby sleep. A 3s hold is an explicit
		 * power-off, so take over the sleep slot: wake the current
		 * sleeper and wait for its resume chain to finish, then enter
		 * deep sleep through the normal (fully resumed) path. Entering
		 * directly from standby would skip callbacks whose suspended
		 * flags are still set and leave the runtime half-alive. */
		LOG_INF("Waking from light/standby sleep to enter deep sleep");
		(void)k_sem_reset(&pm_ctx.resume_done_sem);
		(void)halo_pm_wakeup(HALO_PM_WAKEUP_BUTTON);
		(void)k_sem_take(&pm_ctx.resume_done_sem, K_MSEC(3000));

		if (!atomic_cas(&pm_ctx.is_sleeping, 0, 1)) {
			LOG_WRN("Already in sleep mode, ignoring deep sleep request");
			return -EALREADY;
		}
	}

	LOG_WRN("Preparing deep sleep (BLE OFF) (timeout=%u ms)", timeout_ms);

	pm_ctx.last_mode = HALO_PM_SLEEP_DEEP;

	/* If anything below wedges (a deinit waiting on an event that never
	 * comes), nothing else can recover the device — this thread is the
	 * button thread. The failsafe reboots into a clean state instead. */
	k_work_schedule(&pm_shutdown_failsafe, K_MSEC(PM_SHUTDOWN_FAILSAFE_MS));

	/* Stop filesystem logging for the shutdown: flash writes are slow, and
	 * on a full/inconsistent littlefs the backend enters an unlink-retry
	 * storm whose own error logging feeds it — a permanently runnable
	 * logging thread then starves the idle thread, which blocks even
	 * forced SOFT_OFF entry (observed during a Noa file transfer). UART
	 * still carries the full shutdown log; a boot re-enables the backend. */
	for (int i = 0; i < log_backend_count_get(); i++) {
		const struct log_backend *backend = log_backend_get(i);

		if (backend->name != NULL && strcmp(backend->name, "log_backend_fs") == 0) {
			log_backend_deactivate(backend);
		}
	}

	/* Stop app audio before tearing anything down: a playback thread killed
	 * mid-stream during runtime exit would leak the speaker (and, for
	 * frame.sound, its SOFT_OFF policy lock), making the power-off below
	 * impossible or silencing the shutdown sound. Then confirm the
	 * shutdown audibly while the speaker is still up; the sound also acts
	 * as a settle window between the button release and power-off, so
	 * switch bounce cannot latch a spurious wake event. */
#ifdef CONFIG_HALO_LUA_SPEAKER
	halo_lua_speaker_interrupt();
#endif
#ifdef CONFIG_HALO_SFXR
	(void)halo_sfxr_quiesce(PM_DEEP_AUDIO_QUIESCE_MS);
	(void)halo_shutdown_sound_play();
#endif

	/* Notify all callbacks about suspend (unlocked — see pm_notify_suspend) */
	ret = pm_notify_suspend(HALO_PM_SLEEP_DEEP);
	if (ret < 0) {
		LOG_ERR("Failed to suspend (ret=%d)", ret);
		pm_notify_resume(HALO_PM_SLEEP_DEEP);
		pm_ctx.last_mode = HALO_PM_SLEEP_NONE;
		atomic_set(&pm_ctx.is_sleeping, 0);
		k_work_cancel_delayable(&pm_shutdown_failsafe);
#ifdef CONFIG_HALO_SFXR
		halo_sfxr_quiesce_cancel();
#endif
		return ret;
	}

	LOG_INF("Deep sleep suspend sequence complete, entering sleep state");

	/* From here to power-off, flush logs synchronously: the deferred log
	 * thread gets no chance to drain before the SoC goes down, which
	 * silently discards exactly the lines that explain a failed entry. */
	log_panic();

	/* Update statistics */
	pm_ctx.deep_sleep_count++;

	/* Deep sleep semantics: only the button may wake (plus RTC when a timed
	 * wake was requested). In particular the mic AAD line (LPGPIO0) must
	 * not stay armed — any nearby sound (including our own shutdown sound)
	 * trips acoustic detection and immediately re-wakes the SoC. */
	ret = power_mgr_set_wakeup_sources(true, false, timeout_ms != 0);
	LOG_INF("Deep sleep wakeup sources set (ret=%d)", ret);

	/* Suspend chain completed — from here the bounded park below is the
	 * recovery mechanism, so the wedge failsafe is no longer needed. */
	k_work_cancel_delayable(&pm_shutdown_failsafe);

	pm_policy_state_lock_put(PM_STATE_SOFT_OFF, PM_ALL_SUBSTATES);

	/* Enter SOFT_OFF via pm_state_force(), not the idle policy: an
	 * explicit user power-off must not be vetoed by scheduling pressure.
	 * Even after alif_ble_disable(), unnamed BLE host threads left over
	 * from a past connection keep kernel timeouts denser than the state's
	 * 4 ms min-residency, so pm_policy_next_state() never picks SOFT_OFF
	 * (verified: it returns SOFT_OFF when probed with no timeout). The
	 * forced state bypasses the lock/residency checks and is consumed at
	 * the next idle. */
	const struct pm_state_info force_off = {
		.state = PM_STATE_SOFT_OFF,
	};
	(void)pm_state_force(0U, &force_off);

	/* On success the SoC powers off at the next idle and reboots on wake,
	 * so this sleep never returns. Bounded so a blocked entry is detected
	 * instead of leaving the device dark but awake with no way back. */
	if (timeout_ms > 0) {
		k_sleep(K_MSEC(timeout_ms));
	} else {
		k_sleep(K_MSEC(PM_DEEP_ENTRY_GRACE_MS));
	}

	/* Still executing: even forced entry did not power off, which points
	 * at SoC/SE-level refusal (a device rejecting suspend, or the SE not
	 * granting subsystem-off). Dump forensics and reboot into a clean
	 * running state — the boot logo/sound make the failure visible. */
	bool lock_held = pm_policy_state_lock_is_active(PM_STATE_SOFT_OFF, PM_ALL_SUBSTATES);
	const struct pm_state_info *next = pm_policy_next_state(0U, -1);

	LOG_ERR("Forced SOFT_OFF did not power off within %d ms: lock_held=%d "
		"policy_next(no-timeout)=%d",
		(timeout_ms > 0) ? (int)timeout_ms : PM_DEEP_ENTRY_GRACE_MS, lock_held,
		next ? (int)next->state : -1);
	k_thread_foreach_unlocked(pm_dump_thread_cb, NULL);

	pm_policy_state_lock_get(PM_STATE_SOFT_OFF, PM_ALL_SUBSTATES);
	LOG_ERR("Rebooting");
	k_sleep(K_MSEC(100)); /* Let the log drain */
	sys_reboot(SYS_REBOOT_COLD);

	CODE_UNREACHABLE;
	return -ETIMEDOUT;
}

int halo_pm_get_stats(uint32_t *light_count, uint32_t *standby_count, uint32_t *deep_count,
		      halo_pm_sleep_mode_t *last_mode)
{
	if (pm_ctx.initialized != PM_MANAGER_INIT_MAGIC) {
		return -ENODEV;
	}

	k_mutex_lock(&pm_ctx.lock, K_FOREVER);

	if (light_count) {
		*light_count = pm_ctx.light_sleep_count;
	}
	if (standby_count) {
		*standby_count = pm_ctx.standby_count;
	}
	if (deep_count) {
		*deep_count = pm_ctx.deep_sleep_count;
	}
	if (last_mode) {
		*last_mode = pm_ctx.last_mode;
	}

	k_mutex_unlock(&pm_ctx.lock);

	return 0;
}

bool halo_pm_is_sleeping(void)
{
	if (pm_ctx.initialized != PM_MANAGER_INIT_MAGIC) {
		return false;
	}

	/* Lock-free: called from event paths that must never block behind a
	 * thread running the (long) suspend chain. */
	return atomic_get(&pm_ctx.is_sleeping) != 0;
}

int halo_pm_wakeup(halo_pm_wakeup_source_t source)
{
	struct k_thread *sleep_thread = NULL;

	if (pm_ctx.initialized != PM_MANAGER_INIT_MAGIC) {
		return -ENODEV;
	}

	/* Claim the wakeup lock-free; only one waker wins. The mutex below is
	 * only held briefly for the bookkeeping fields — never while the
	 * suspend chain runs, so this cannot block event paths. */
	if (!atomic_cas(&pm_ctx.is_sleeping, 1, 0)) {
		return 0;
	}

	k_mutex_lock(&pm_ctx.lock, K_FOREVER);
	pm_ctx.wakeup_source = source;
	sleep_thread = pm_ctx.sleep_thread;
	k_mutex_unlock(&pm_ctx.lock);

	if (sleep_thread != NULL) {
		k_wakeup(sleep_thread);
	}

	/* Signal wakeup semaphore */
	k_sem_give(&pm_ctx.wakeup_sem);

	LOG_INF("Wakeup signal sent (source: %s)", halo_pm_wakeup_source_name(source));
	return 0;
}

halo_pm_wakeup_source_t halo_pm_get_wakeup_source(void)
{
	if (pm_ctx.initialized != PM_MANAGER_INIT_MAGIC) {
		return HALO_PM_WAKEUP_UNKNOWN;
	}

	halo_pm_wakeup_source_t source;
	k_mutex_lock(&pm_ctx.lock, K_FOREVER);
	source = pm_ctx.wakeup_source;
	k_mutex_unlock(&pm_ctx.lock);

	return source;
}

const char *halo_pm_wakeup_source_name(halo_pm_wakeup_source_t source)
{
	switch (source) {
	case HALO_PM_WAKEUP_UNKNOWN:
		return "unknown";
	case HALO_PM_WAKEUP_TIMEOUT:
		return "timeout";
	case HALO_PM_WAKEUP_BUTTON:
		return "button";
	case HALO_PM_WAKEUP_BLE:
		return "ble";
	case HALO_PM_WAKEUP_IMU:
		return "imu";
	case HALO_PM_WAKEUP_MICROPHONE:
		return "microphone";
	case HALO_PM_WAKEUP_EXTERNAL:
		return "external";
	case HALO_PM_WAKEUP_WATCHDOG:
		return "watchdog";
	default:
		return "invalid";
	}
}

int halo_pm_prevent_sleep(void)
{
	if (pm_ctx.initialized != PM_MANAGER_INIT_MAGIC) {
		return -ENODEV;
	}

	pm_policy_state_lock_get(PM_STATE_SOFT_OFF, PM_ALL_SUBSTATES);
	return 0;
}

int halo_pm_allow_sleep(void)
{
	if (pm_ctx.initialized != PM_MANAGER_INIT_MAGIC) {
		return -ENODEV;
	}

	pm_policy_state_lock_put(PM_STATE_SOFT_OFF, PM_ALL_SUBSTATES);
	return 0;
}

halo_pm_sleep_mode_t halo_pm_get_sleep_mode()
{
	halo_pm_sleep_mode_t mode = HALO_PM_SLEEP_NONE;
	if (pm_ctx.initialized != PM_MANAGER_INIT_MAGIC) {
		return mode;
	}
	mode = pm_ctx.last_mode;
	return mode;
}