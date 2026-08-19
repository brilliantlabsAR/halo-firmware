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

#ifdef __cplusplus
}
#endif

#endif /* HALO_BLE_AUDIO_H */
