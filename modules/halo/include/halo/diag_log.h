/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_DIAG_LOG_H
#define HALO_DIAG_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Diagnostic log for bring-up experiments (CONFIG_HALO_DIAG_LOG)
 *
 * A short, explicit event log in /lfs/diag.txt: one line per boot stage,
 * peripheral wake and heartbeat, each stamped with the uptime and synced at
 * once, so the file shows how far a unit got before it lost power or reset.
 * When the file reaches CONFIG_HALO_DIAG_LOG_MAX_BYTES it moves to
 * /lfs/diag.old (replacing the previous one) and a new file starts.
 *
 * Settings come from /lfs/diag.cfg, "key=value" lines read at init (see
 * halo_diag_setting()). Lua can write that file to change an experiment
 * without rebuilding.
 */

/**
 * @brief Open the log file, read /lfs/diag.cfg and write out the lines
 * logged before the file system was up. Call after halo_file_init().
 */
int halo_diag_init(void);

/**
 * @brief Append one line ("[uptime ms] text") to the log and sync it.
 * Before halo_diag_init() lines are held in RAM. Also logged at INF level.
 */
void halo_diag_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/**
 * @brief The integer value of a key in /lfs/diag.cfg, or def if absent.
 */
int halo_diag_setting(const char *key, int def);

/**
 * @brief A fresh battery reading in mV (takes ~70 ms), or -1.
 */
int halo_diag_vbat_mv(void);

/**
 * @brief The battery manager's last reading in mV (no delay), or -1.
 */
int halo_diag_vbat_cached_mv(void);

/**
 * @brief Log the uptime and battery every seconds (0 stops it).
 */
void halo_diag_heartbeat(int seconds);

/**
 * @brief Read the battery back to back (every period_ms at most) until
 * halo_diag_vbat_watch_stop(), keeping the lowest reading.
 */
void halo_diag_vbat_watch_start(int period_ms);

/**
 * @brief Stop the watch; the lowest reading in mV and the number of
 * readings taken (-1 and 0 if none finished).
 */
int halo_diag_vbat_watch_stop(int *samples);

#ifdef __cplusplus
}
#endif

#endif /* HALO_DIAG_LOG_H */
