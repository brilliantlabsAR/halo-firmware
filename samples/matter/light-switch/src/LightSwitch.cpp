/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include "LightSwitch.h"
#include "AppEvent.h"
#include "BindingHandler.h"
#include "ShellCommands.h"

#include <app/server/Server.h>
#include <app/util/binding-table.h>
#include <controller/InvokeInteraction.h>
#include <app/clusters/switch-server/switch-server.h>
#include <app/clusters/on-off-server/on-off-server.h>

#include <app-common/zap-generated/attributes/Accessors.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;

void LightSwitch::Init(chip::EndpointId aLightDimmerSwitchEndpoint, chip::EndpointId aLightGenericSwitchEndpointId)
{
	BindingHandler::GetInstance().Init();

	/* Enable shell commands */
	SwitchCommands::RegisterSwitchCommands();
	mLightSwitchEndpoint = aLightDimmerSwitchEndpoint;
	mLightGenericSwitchEndpointId = aLightGenericSwitchEndpointId;	
}

void LightSwitch::LightOnOffServerControl(Action mAction)
{
	chip::CommandId command;

	switch (mAction) {
	case Action::Toggle:
		command = Clusters::OnOff::Commands::Toggle::Id;
		break;
	case Action::On:
		command = Clusters::OnOff::Commands::On::Id;
		break;

	default:
		command = Clusters::OnOff::Commands::Off::Id;
		break;
	}

	OnOffServer::Instance().setOnOffValue(mLightSwitchEndpoint, command, false);
}

void LightSwitch::LightControl(Action mAction)
{
	BindingHandler::BindingData *data = Platform::New<BindingHandler::BindingData>();
	if (data) {
		data->EndpointId = mLightSwitchEndpoint;
		data->ClusterId = Clusters::OnOff::Id;
		data->BindCommandFunc = LightSwitchChangedHandler;
		switch (mAction) {
		case Action::Toggle:
			data->CommandId = Clusters::OnOff::Commands::Toggle::Id;
			break;
		case Action::On:
			data->CommandId = Clusters::OnOff::Commands::On::Id;
			break;
		case Action::Off:
			data->CommandId = Clusters::OnOff::Commands::Off::Id;
			break;
		default:
			Platform::Delete(data);
			return;
		}
		data->IsGroup = BindingHandler::GetInstance().IsGroupBound();
		BindingHandler::SheduleClusterAction(data);
	}
}

void LightSwitch::LightDimmControl(uint16_t dimmLevel)
{

	BindingHandler::BindingData *data = Platform::New<BindingHandler::BindingData>();
	if (data) {
		data->EndpointId = mLightSwitchEndpoint;
		data->CommandId = Clusters::LevelControl::Commands::MoveToLevel::Id;
		data->ClusterId = Clusters::LevelControl::Id;
		data->BindCommandFunc = LightSwitchChangedHandler;

		if (dimmLevel > kMaximumBrightness) {
			dimmLevel = kMaximumBrightness;
		}
		data->Value = static_cast<uint8_t>(dimmLevel);
		data->IsGroup = BindingHandler::GetInstance().IsGroupBound();
		BindingHandler::SheduleClusterAction(data);
	}
}

void LightSwitch::OnOffProcessCommand(CommandId aCommandId, const EmberBindingTableEntry &aBinding,
				      OperationalDeviceProxy *aDevice,
				      BindingHandler::BindingData &bindingData)
{
	CHIP_ERROR ret = CHIP_NO_ERROR;

	auto onSuccess = [dataPointer = Platform::New<BindingHandler::BindingData>(bindingData)](
				 const ConcreteCommandPath &commandPath, const StatusIB &status,
				 const auto &dataResponse) {
		BindingHandler::BindCommandSucces(dataPointer);
	};

	auto onFailure = [dataPointer = Platform::New<BindingHandler::BindingData>(bindingData)](
				 CHIP_ERROR aError) {
		BindingHandler::BindCommandFailure(dataPointer, aError);
	};

	/* Check that connection is ready for active device */
	if (aDevice) {
		VerifyOrDie(aDevice->ConnectionReady());
	}

	switch (aCommandId) {
	case Clusters::OnOff::Commands::Toggle::Id:
		Clusters::OnOff::Commands::Toggle::Type toggleCommand;
		if (aDevice) {
			ret = Controller::InvokeCommandRequest(
				aDevice->GetExchangeManager(), aDevice->GetSecureSession().Value(),
				aBinding.remote, toggleCommand, onSuccess, onFailure);
		} else {

			Messaging::ExchangeManager &exchangeMgr =
				Server::GetInstance().GetExchangeManager();
			ret = Controller::InvokeGroupCommandRequest(
				&exchangeMgr, aBinding.fabricIndex, aBinding.groupId,
				toggleCommand);
		}
		break;

	case Clusters::OnOff::Commands::On::Id:
		Clusters::OnOff::Commands::On::Type onCommand;
		if (aDevice) {
			ret = Controller::InvokeCommandRequest(
				aDevice->GetExchangeManager(), aDevice->GetSecureSession().Value(),
				aBinding.remote, onCommand, onSuccess, onFailure);
		} else {
			Messaging::ExchangeManager &exchangeMgr =
				Server::GetInstance().GetExchangeManager();
			ret = Controller::InvokeGroupCommandRequest(
				&exchangeMgr, aBinding.fabricIndex, aBinding.groupId, onCommand);
		}
		break;

	case Clusters::OnOff::Commands::Off::Id:
		Clusters::OnOff::Commands::Off::Type offCommand;
		if (aDevice) {
			ret = Controller::InvokeCommandRequest(
				aDevice->GetExchangeManager(), aDevice->GetSecureSession().Value(),
				aBinding.remote, offCommand, onSuccess, onFailure);
		} else {
			Messaging::ExchangeManager &exchangeMgr =
				Server::GetInstance().GetExchangeManager();
			ret = Controller::InvokeGroupCommandRequest(
				&exchangeMgr, aBinding.fabricIndex, aBinding.groupId, offCommand);
		}

		break;
	default:
		LOG_DBG("Invalid binding command data - commandId is not supported");
		break;
	}
	if (CHIP_NO_ERROR != ret) {
		LOG_ERR("Invoke OnOff Command Request ERROR: %s", ErrorStr(ret));
	}
}

void LightSwitch::LevelControlProcessCommand(CommandId aCommandId,
					     const EmberBindingTableEntry &aBinding,
					     OperationalDeviceProxy *aDevice,
					     BindingHandler::BindingData &bindingData)
{
	auto onSuccess = [dataPointer = Platform::New<BindingHandler::BindingData>(bindingData)](
				 const ConcreteCommandPath &commandPath, const StatusIB &status,
				 const auto &dataResponse) {
		BindingHandler::BindCommandSucces(dataPointer);
	};

	auto onFailure = [dataPointer = Platform::New<BindingHandler::BindingData>(bindingData)](
				 CHIP_ERROR aError) {
		BindingHandler::BindCommandFailure(dataPointer, aError);
	};

	CHIP_ERROR ret = CHIP_NO_ERROR;

	/* Check that connection is ready for active device */
	if (aDevice) {
		VerifyOrDie(aDevice->ConnectionReady());
	}

	switch (aCommandId) {
	case Clusters::LevelControl::Commands::MoveToLevel::Id: {
		Clusters::LevelControl::Commands::MoveToLevel::Type moveToLevelCommand;
		moveToLevelCommand.level = bindingData.Value;
		if (aDevice) {
			ret = Controller::InvokeCommandRequest(
				aDevice->GetExchangeManager(), aDevice->GetSecureSession().Value(),
				aBinding.remote, moveToLevelCommand, onSuccess, onFailure);
		} else {
			Messaging::ExchangeManager &exchangeMgr =
				Server::GetInstance().GetExchangeManager();
			ret = Controller::InvokeGroupCommandRequest(
				&exchangeMgr, aBinding.fabricIndex, aBinding.groupId,
				moveToLevelCommand);
		}
	} break;
	default:
		LOG_DBG("Invalid binding command data - commandId is not supported");
		break;
	}
	if (CHIP_NO_ERROR != ret) {
		LOG_ERR("Invoke Group Command Request ERROR: %s", ErrorStr(ret));
	}
}

void LightSwitch::LightSwitchChangedHandler(const EmberBindingTableEntry &binding,
					    OperationalDeviceProxy *deviceProxy,
					    BindingHandler::BindingData &bindingData)
{
	OperationalDeviceProxy *temp_proxy;
	chip::ClusterId ClusterId = bindingData.ClusterId;

	if (binding.type == MATTER_MULTICAST_BINDING && bindingData.IsGroup) {
		/* Multicast should not use device proxy */
		temp_proxy = nullptr;
	} else if (binding.type == MATTER_UNICAST_BINDING && !bindingData.IsGroup) {
		temp_proxy = deviceProxy;
	} else {
		ClusterId = 0;
	}

	switch (ClusterId) {
	case Clusters::OnOff::Id:
		OnOffProcessCommand(bindingData.CommandId, binding, temp_proxy, bindingData);
		break;
	case Clusters::LevelControl::Id:
		LevelControlProcessCommand(bindingData.CommandId, binding, temp_proxy, bindingData);
		break;
	default:
		LOG_ERR("Invalid binding type %d command data", binding.type);
		break;
	}
}

void LightSwitch::GenericSwitchInitialPress()
{
    DeviceLayer::SystemLayer().ScheduleLambda([this] {
        // Press moves Position from 0 (idle) to 1 (press)
        uint8_t newPosition = 1;

        Clusters::Switch::Attributes::CurrentPosition::Set(mLightGenericSwitchEndpointId, newPosition);
        // InitialPress event takes newPosition as event data
        Clusters::SwitchServer::Instance().OnInitialPress(mLightGenericSwitchEndpointId, newPosition);
    });
}

void LightSwitch::GenericSwitchReleasePress()
{
    DeviceLayer::SystemLayer().ScheduleLambda([this] {
        // Release moves Position from 1 (press) to 0 (idle)
        uint8_t previousPosition = 1;
        uint8_t newPosition      = 0;

        Clusters::Switch::Attributes::CurrentPosition::Set(mLightGenericSwitchEndpointId, newPosition);
        // ShortRelease event takes previousPosition as event data
        Clusters::SwitchServer::Instance().OnShortRelease(mLightGenericSwitchEndpointId, previousPosition);
    });
}
