/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HALO_BLE_AUDIO_H
#define HALO_BLE_AUDIO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file ble_audio.h
 * @brief LE Audio Unicast Sink API for Halo
 *
 * This module implements LE Audio Unicast Sink functionality, allowing
 * the device to receive audio streams from LE Audio sources (like phones).
 *
 * Key components:
 * - BAP (Basic Audio Profile) Unicast Server
 * - PACS (Published Audio Capabilities Server) - advertises supported codecs
 * - ASCS (Audio Stream Control Service) - manages audio streams
 * - TMAP (Telephony and Media Audio Profile) - role advertisement
 * - VCS (Volume Control Service) - remote volume control
 *
 * Note: This module does NOT handle BLE advertising. Use the existing
 * halo_ble_conn_adv_start() from ble_connection.h for advertising.
 *
 * Typical usage flow:
 * 1. Call halo_ble_conn_init() to initialize BLE stack
 * 2. Call halo_ble_audio_init() to initialize the LE Audio services
 * 3. Call halo_ble_conn_adv_start() to begin advertising
 * 4. Client connects and configures audio streams
 * 5. Audio data flows automatically through the audio datapath
 * 6. Call halo_ble_audio_deinit() when done
 */

/**
 * @brief Initialize the LE Audio service
 *
 * Initializes all LE Audio components including:
 * - BAP Unicast Server (Audio Stream Control Service)
 * - PACS (Published Audio Capabilities)
 * - TMAP (Telephony and Media Audio Profile)
 * - VCS (Volume Control Service)
 *
 * This must be called after BLE stack initialization (halo_ble_conn_init)
 * and before starting advertising.
 *
 * @param reset If true, reset the service state even if already initialized.
 *              Useful for recovery from error states.
 * @return 0 on success, negative error code on failure
 */
int halo_ble_audio_init(bool reset);

/**
 * @brief Deinitialize the LE Audio service
 *
 * Releases all resources allocated by halo_ble_audio_init().
 * Should be called when LE Audio functionality is no longer needed.
 *
 * @return 0 on success, negative error code on failure
 */
int halo_ble_audio_deinit(void);

/**
 * @brief Get the TMAP role bit field this device exposes
 *
 * Matches the roles configured in TMAS. Intended for embedding the TMAS
 * service data (UUID 0x1855 + Role) in advertising/scan-response payloads.
 *
 * @return TMAP role bit field (TMAP_ROLE_* bits)
 */
uint16_t halo_ble_audio_get_tmap_roles(void);

/**
 * @brief Get the supported audio context bit fields
 *
 * Matches the contexts registered with PACS. Intended for embedding the ASCS
 * announcement (UUID 0x184E + announcement type + available contexts) in
 * advertising/scan-response payloads.
 *
 * @param sink_context_bf Receives the sink (playback) context bit field; may be NULL
 * @param src_context_bf Receives the source (capture) context bit field; may be NULL
 */
void halo_ble_audio_get_context_types(uint16_t *sink_context_bf, uint16_t *src_context_bf);

/** LE Audio availability and activity, per direction */
struct halo_ble_audio_availability {
	bool sink_available;   /**< Speaker (sink) contexts are advertised as available */
	bool source_available; /**< Microphone (source) contexts are available */
	bool sink_active;      /**< A sink ASE is enabling, streaming or disabling: LE Audio holds the speaker */
	bool source_active;    /**< A source ASE is enabling, streaming or disabling: LE Audio holds the mic */
};

/**
 * @brief Make LE Audio available or unavailable, per direction, at runtime
 *
 * Sets the PACS Available Audio Contexts for all connections (0 for an
 * unavailable direction, all supported contexts otherwise) and refuses an
 * ASE Enable for an unavailable direction. A stream already enabled is not
 * torn down. Not persisted: every boot starts with both available.
 *
 * @param sink   Speaker (sink) available
 * @param source Microphone (source) available
 * @return 0 on success, -EIO if the PACS server refused the update
 */
int halo_ble_audio_set_available(bool sink, bool source);

/**
 * @brief Read the runtime availability and whether LE Audio holds the hardware
 *
 * @param out Receives the state
 */
void halo_ble_audio_get_available(struct halo_ble_audio_availability *out);

/**
 * @brief Get the speaker levels applied to LE Audio sink streams
 *
 * The effective values: a persisted runtime override if one is set,
 * otherwise HALO_BLE_AUDIO_SINK_BUDGET / HALO_BLE_AUDIO_SINK_GAIN_DB.
 *
 * @param budget Receives the energy budget (0 = speaker default); may be NULL
 * @param gain_db Receives the digital pre-gain in dB; may be NULL
 */
void halo_ble_audio_sink_levels_get(int *budget, int *gain_db);

/**
 * @brief Persist the speaker levels applied to LE Audio sink streams
 *
 * Takes effect at the next sink stream start. -1 clears that override and
 * restores the build default.
 *
 * @param budget 0 (speaker default) or 10..100, or -1
 * @param gain_db 0..12 dB, or -1
 * @return 0 on success, -EINVAL on an out-of-range value
 */
int halo_ble_audio_sink_levels_set(int budget, int gain_db);

#ifdef __cplusplus
}
#endif

#endif /* HALO_BLE_AUDIO_H */
