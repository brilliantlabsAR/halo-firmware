/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#pragma once

#include <app-common/zap-generated/ids/Clusters.h>
#include <app-common/zap-generated/ids/Commands.h>
#include <app/CommandSender.h>
#include <app/clusters/bindings/BindingManager.h>
#include <controller/InvokeInteraction.h>
#include <platform/CHIPDeviceLayer.h>

class BindingHandler
{
      public:

	struct BindingData;
      	using BindCommand = void (*)(const EmberBindingTableEntry &, chip::OperationalDeviceProxy *,
					       BindingData &);
	struct BindingData {
		chip::EndpointId EndpointId;
		chip::CommandId CommandId;
		chip::ClusterId ClusterId;
		BindCommand BindCommandFunc;
		uint8_t Value;
		bool IsGroup{false};
		bool mRecoverActive{false};
	};

	void Init();
	const void PrintTable();
	const bool IsGroupBound();
	
	static void SheduleClusterAction(BindingData *bindingData);
	static void BindCommandFailure(BindingData *bindingData, CHIP_ERROR aError);
	static void BindCommandSucces(BindingData *bindingData);

	static BindingHandler &GetInstance()
	{
		static BindingHandler sBindingHandler;
		return sBindingHandler;
	}

      private:
	BindingHandler() = default;
	static void ClusterChangedCallback(const EmberBindingTableEntry &binding,
						  chip::OperationalDeviceProxy *deviceProxy, void *context);
	static void BindWorkerHandler(intptr_t);
	static void BindContextReleaseHandler(void *context);
	static void InitInternal(intptr_t);

};
