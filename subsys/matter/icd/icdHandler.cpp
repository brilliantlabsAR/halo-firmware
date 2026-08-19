/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include "icdHandler.h"
#include "MatterStack.h"
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

CHIP_ERROR ICDHandler::OnSubscriptionRequested(chip::app::ReadHandler &aReadHandler,
					    chip::Transport::SecureSession &aSecureSession)
{
	uint16_t agreedMaxInterval = kSubscriptionMaxIntervalPublisherLimit;
	uint16_t requestedMinInterval = 0;
	uint16_t requestedMaxInterval = 0;
	aReadHandler.GetReportingIntervals(requestedMinInterval, requestedMaxInterval);

	if (requestedMaxInterval < agreedMaxInterval) {
		agreedMaxInterval = requestedMaxInterval;
	}

        LOG_INF("SuBscribe Request %u min %u max",  requestedMinInterval, requestedMaxInterval);
	return aReadHandler.SetMaxReportingInterval(agreedMaxInterval);
}

void ICDHandler::OnSubscriptionEstablished(chip::app::ReadHandler &aReadHandler)
{
        MatterStack::Instance().MatterEndpointSubscripted();
        LOG_INF("SubScRibe Established");
}

void ICDHandler::OnSubscriptionTerminated(chip::app::ReadHandler &aReadHandler)
{
        LOG_INF("SubScRibe terminated");
}