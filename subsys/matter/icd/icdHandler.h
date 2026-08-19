/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#pragma once

#include <app/ReadHandler.h>

class ICDHandler : public chip::app::ReadHandler::ApplicationCallback
{
    
	CHIP_ERROR OnSubscriptionRequested(chip::app::ReadHandler &aReadHandler,
					   chip::Transport::SecureSession &aSecureSession) override;
	void OnSubscriptionEstablished(chip::app::ReadHandler &aReadHandler) override;

	void OnSubscriptionTerminated(chip::app::ReadHandler &aReadHandler) override;

    public:
    static ICDHandler &Instance()
	{
		static ICDHandler sICDInstance;
		return sICDInstance;
	};

};
