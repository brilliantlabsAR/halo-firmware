/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#ifndef _ISO_DATAPATH_HTOC_H
#define _ISO_DATAPATH_HTOC_H

/**
 * @file
 * @brief BLE isochronous data path in Host --> Controller direction
 */

#include <zephyr/kernel.h>
#include "sdu_queue.h"

/**
 * @brief Create an instance of isochronous datapath and bind it to a stream
 *
 * @param stream_lid The stream local ID on the controller to bind with
 * @param sdu_queue The SDU queue to pull SDUs from
 * @param timing_master_channel true if this channel is the "master" channel used to control
 * presentation delay
 *
 * @retval The created ISO datapath instance if successful
 * @retval NULL on failure
 */
struct iso_datapath_htoc *iso_datapath_htoc_create(uint8_t stream_lid, struct sdu_queue *sdu_queue,
						   bool timing_master_channel);

/**
 * @brief Initialize an instance of isochronous datapath from host to controller (source)
 *
 * @param stream_lid The stream local ID on the controller to bind with
 * @param sdu_queue The SDU queue to pull SDUs from
 * @param timing_master_channel true if this channel is the "master" channel used to control
 * presentation delay
 *
 * @retval The created ISO datapath instance if successful
 * @retval NULL on failure
 */
struct iso_datapath_htoc *iso_datapath_htoc_init(uint8_t stream_lid, struct sdu_queue *sdu_queue,
						 bool timing_master_channel);

/**
 * @brief Bind the datapath to the controller
 *
 * @param datapath The datapath instance to bind
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int iso_datapath_htoc_bind(struct iso_datapath_htoc *datapath);

/**
 * @brief Unbind the datapath from the controller
 *
 * @param datapath The datapath instance to unbind
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int iso_datapath_htoc_unbind(struct iso_datapath_htoc *datapath);

/**
 * @brief Notify iso datapath that a new SDU is available
 *
 * @param datapath Pointer to datapath that SDU is available for
 * @param capture_timestamp Timestamp at which the audio data contained in the SDU was captured
 * @param sdu_seq Sequence number of SDU
 */
void iso_datapath_htoc_notify_sdu_available(void *datapath, uint32_t capture_timestamp,
					    uint16_t sdu_seq);

/**
 * @brief Delete an instance of isochronous datapath in host --> controller direction
 *
 * @param datapath The datapath instance to delete
 *
 * @retval 0 if successful
 * @retval Negative error code on failure
 */
int iso_datapath_htoc_delete(struct iso_datapath_htoc *datapath);

#endif /* _ISO_DATAPATH_HTOC_H */
