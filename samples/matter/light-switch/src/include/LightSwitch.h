/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#pragma once

#include <app/util/basic-types.h>
#include <lib/core/CHIPError.h>
#include "BindingHandler.h"

class LightSwitch
{
      public:
	enum class Action : uint8_t {
		Toggle, /// Switch state on lighting-app device
		On,     /// Turn on light on lighting-app device
		Off     /// Turn off light on lighting-app device
	};

	void Init(chip::EndpointId aLightDimmerSwitchEndpoint, chip::EndpointId aLightGenericSwitchEndpointId);
	void LightOnOffServerControl(Action);
	void LightControl(Action);
	void LightDimmControl(uint16_t brigtness);
	void GenericSwitchInitialPress();
	void GenericSwitchReleasePress();
	static void LightSwitchChangedHandler(const EmberBindingTableEntry &binding,
					      chip::OperationalDeviceProxy *deviceProxy,
					      BindingHandler::BindingData &bindingData);

	static LightSwitch &GetInstance()
	{
		static LightSwitch sLightSwitch;
		return sLightSwitch;
	}

      private:
	LightSwitch() = default;
	static void OnOffProcessCommand(chip::CommandId, const EmberBindingTableEntry &,
					chip::OperationalDeviceProxy *,
					BindingHandler::BindingData &bindingData);
	static void LevelControlProcessCommand(chip::CommandId, const EmberBindingTableEntry &,
					       chip::OperationalDeviceProxy *,
					       BindingHandler::BindingData &bindingData);
	constexpr static auto kMaximumBrightness = 254;

	chip::EndpointId mLightSwitchEndpoint;
	chip::EndpointId mLightGenericSwitchEndpointId;
};
