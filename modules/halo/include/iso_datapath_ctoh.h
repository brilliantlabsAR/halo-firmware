/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef _ISO_DATAPATH_CTOH_H
#define _ISO_DATAPATH_CTOH_H

/**
 * @file
 * @brief BLE isochronous data path in Controller --> Host direction
 */

#include <zephyr/kernel.h>
#include "sdu_queue.h"

/**
 * @brief Initialize an instance of isochronous datapath and bind it to a stream
 *
 * @param stream_lid The stream local ID on the controller to bind with
 * @param sdu_queue The SDU queue to send SDUs to
 *
 * @retval The created ISO datapath instance if successful
 * @retval NULL on failure
 */
struct iso_datapath_ctoh *iso_datapath_ctoh_create(uint8_t stream_lid, struct sdu_queue *sdu_queue);

/**
 * @brief Initialize an instance of isochronous datapath from controller to host (sink)
 *
 * @param stream_lid The stream local ID on the controller to bind with
 * @param sdu_queue The SDU queue to send SDUs to
 *
 * @retval The created ISO datapath instance if successful
 * @retval NULL on failure
 */
struct iso_datapath_ctoh *iso_datapath_ctoh_init(uint8_t stream_lid, struct sdu_queue *sdu_queue);

/**
 * @brief Start fetching packets from isochronous stream
 *
 * @param datapath The datapath instance to start
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int iso_datapath_ctoh_start(struct iso_datapath_ctoh *datapath);

/**
 * @brief Stop fetching packets from isochronous stream
 *
 * @param datapath The datapath instance to stop
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int iso_datapath_ctoh_stop(struct iso_datapath_ctoh *datapath);

/**
 * @brief Notify ISO datapath that an SDU has been processed
 *        This is used to trigger next SDU read from controller if the
 *        SDU queue was empty for some reason.
 *
 * @param datapath Pointer to datapath associated with the processed SDU
 * @param timestamp Timestamp of the SDU
 * @param sdu_seq Sequence number of SDU
 */
void iso_datapath_ctoh_notify_sdu_done(void *datapath, uint32_t timestamp, uint16_t sdu_seq);

/**
 * @brief Delete an instance of isochronous datapath
 *
 * @param datapath The datapath instance to delete
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int iso_datapath_ctoh_delete(struct iso_datapath_ctoh *datapath);

#endif /* _ISO_DATAPATH_CTOH_H */
