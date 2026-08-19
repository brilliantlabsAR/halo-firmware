/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include "BindingHandler.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace chip;
using namespace chip::app;

void BindingHandler::Init()
{
	DeviceLayer::PlatformMgr().ScheduleWork(InitInternal);
}

void BindingHandler::SheduleClusterAction(BindingData *bindingData)
{
	VerifyOrReturn(bindingData != nullptr, LOG_ERR("Invalid binding data"));
	VerifyOrReturn(bindingData->BindCommandFunc != nullptr,
		       LOG_ERR("No valid InvokeCommandFunc assigned"););

	DeviceLayer::PlatformMgr().ScheduleWork(BindWorkerHandler,
						reinterpret_cast<intptr_t>(bindingData));
}

void BindingHandler::BindCommandSucces(BindingData *bindingData)
{
	LOG_INF("Binding command applied successfully!");

	/* If session was recovered and communication works, reset flag to the initial state. */
	if (bindingData->mRecoverActive) {
		bindingData->mRecoverActive = false;
	}
	Platform::Delete<BindingData>(bindingData);
}

void BindingHandler::BindCommandFailure(BindingData *BindingData, CHIP_ERROR aError)
{
	CHIP_ERROR error;

	if (aError == CHIP_ERROR_TIMEOUT && !BindingData->mRecoverActive) {
		LOG_INF("Response timeout for invoked command");
		BindingData->mRecoverActive = true;

		// Establish new CASE session and retrasmit command that was not applied.
		error = BindingManager::GetInstance().NotifyBoundClusterChanged(
			BindingData->EndpointId, BindingData->ClusterId,
			BindingData);

		if (CHIP_NO_ERROR != error) {
			LOG_ERR("NotifyBoundClusterChanged failed due to: %" CHIP_ERROR_FORMAT,
				error.Format());
			return;
		}
	} else {
		LOG_ERR("Binding command was not applied! Reason: %" CHIP_ERROR_FORMAT,
			aError.Format());
	}
}

void BindingHandler::ClusterChangedCallback(const EmberBindingTableEntry &binding,
					    OperationalDeviceProxy *deviceProxy, void *context)
{
	VerifyOrReturn(context != nullptr, LOG_ERR("Invalid context for device handler"));
	BindingData *data = static_cast<BindingData *>(context);

	if (binding.type == MATTER_MULTICAST_BINDING && data->IsGroup) {
		data->BindCommandFunc(binding, nullptr, *data);
	} else if (binding.type == MATTER_UNICAST_BINDING && !(data->IsGroup)) {
		data->BindCommandFunc(binding, deviceProxy, *data);
	}
}

void BindingHandler::BindContextReleaseHandler(void *context)
{
	VerifyOrReturn(context != nullptr,
		       LOG_ERR("Invalid context for Light switch context release handler"););

	Platform::Delete(static_cast<BindingData *>(context));
}

void BindingHandler::InitInternal(intptr_t aArg)
{
	LOG_INF("Initialize binding Handler");
	auto &server = Server::GetInstance();

	if (CHIP_NO_ERROR != BindingManager::GetInstance().Init({&server.GetFabricTable(),
								 server.GetCASESessionManager(),
								 &server.GetPersistentStorage()})) {
		LOG_ERR("BindingHandler::InitInternal failed");
	}

	// Register Update and release handler
	BindingManager::GetInstance().RegisterBoundDeviceChangedHandler(ClusterChangedCallback);
	BindingManager::GetInstance().RegisterBoundDeviceContextReleaseHandler(
		BindContextReleaseHandler);
}

const bool BindingHandler::IsGroupBound()
{
	BindingTable &bindingTable = BindingTable::GetInstance();

	for (const auto &entry : bindingTable) {
		if (MATTER_MULTICAST_BINDING == entry.type) {
			return true;
		}
	}
	return false;
}

const void BindingHandler::PrintTable()
{
	uint8_t i = 0;
	BindingTable &bindingTable = BindingTable::GetInstance();

	LOG_INF("Binding Table size: [%d]:", bindingTable.Size());
	for (const auto &entry : bindingTable) {
		switch (entry.type) {
		case MATTER_UNICAST_BINDING:
			LOG_INF("[%d] UNICAST: Fabric: %d LocalEndpoint %d ClusterId %d "
				"RemoteEndpointId %d NodeId %d",
				i++, (int)entry.fabricIndex, (int)entry.local,
				(int)entry.clusterId.value_or(kInvalidClusterId), (int)entry.remote,
				(int)entry.nodeId);
			break;
		case MATTER_MULTICAST_BINDING:
			LOG_INF("[%d] GROUP: Fabric: %d LocalEndpoint %d RemoteEndpointId %d "
				"GroupId %d",
				i++, (int)entry.fabricIndex, (int)entry.local, (int)entry.remote,
				(int)entry.groupId);
			break;
		case MATTER_UNUSED_BINDING:
			LOG_INF("[%d] UNUSED", i++);
			break;
		default:
			break;
		}
	}
}

void BindingHandler::BindWorkerHandler(intptr_t aContext)
{
	VerifyOrReturn(aContext != 0, LOG_ERR("Invalid Swich data"));

	BindingData *data = reinterpret_cast<BindingData *>(aContext);
	LOG_INF("Notify Bounded Cluster | endpoint: %d cluster: %d", data->EndpointId,
		data->ClusterId);
	BindingManager::GetInstance().NotifyBoundClusterChanged(data->EndpointId, data->ClusterId,
								data);
}
