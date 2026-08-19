/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include "AppTask.h"

#include "AppConfig.h"
#include "MatterStack.h"
#include "MatterUi.h"
#include "PWMDevice.h"
#include "icdHandler.h"
#include <app/InteractionModelEngine.h>
#include <DeviceInfoProviderImpl.h>
#include <app-common/zap-generated/attributes/Accessors.h>
#include <app/DeferredAttributePersistenceProvider.h>
#include <app/clusters/identify-server/identify-server.h>
#include <app/server/Dnssd.h>
#include <app/server/OnboardingCodesUtil.h>
#include <app/server/Server.h>
#include <credentials/DeviceAttestationCredsProvider.h>
#include <credentials/examples/DeviceAttestationCredsExample.h>
#include <lib/core/ErrorStr.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/CodeUtils.h>
#include <system/SystemClock.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;

namespace
{

constexpr EndpointId kLightEndpointId = 1;
constexpr uint8_t kDefaultMinLevel = 0;
constexpr uint8_t kDefaultMaxLevel = 254;
constexpr int kAppEventQueueSize = 10;

K_MSGQ_DEFINE(sAppEventQueue, sizeof(AppEvent), kAppEventQueueSize, alignof(AppEvent));

// Create Identify server
Identify sIdentify = {kLightEndpointId, AppTask::IdentifyStartHandler, AppTask::IdentifyStopHandler,
		      Clusters::Identify::IdentifyTypeEnum::kVisibleIndicator};

chip::DeviceLayer::DeviceInfoProviderImpl gExampleDeviceInfoProvider;

DeferredAttribute gCurrentLevelPersister(
	ConcreteAttributePath(kLightEndpointId, Clusters::LevelControl::Id,
			      Clusters::LevelControl::Attributes::CurrentLevel::Id));
DeferredAttributePersistenceProvider
	gDeferredAttributePersister(Server::GetInstance().GetDefaultAttributePersister(),
				    Span<DeferredAttribute>(&gCurrentLevelPersister, 1),
				    System::Clock::Milliseconds32(5000));

const struct pwm_dt_spec sLightPwmDevice = PWM_DT_SPEC_GET(DT_ALIAS(pwm_led0));

} // namespace

CHIP_ERROR AppTask::DevInit()
{
	LOG_INF("Init Lighting-app cluster");

	// Initialize lighting device (PWM)
	uint8_t minLightLevel = kDefaultMinLevel;
	Clusters::LevelControl::Attributes::MinLevel::Get(kLightEndpointId, &minLightLevel);

	uint8_t maxLightLevel = kDefaultMaxLevel;
	Clusters::LevelControl::Attributes::MaxLevel::Get(kLightEndpointId, &maxLightLevel);

	uint8_t current = 0;
	app::DataModel::Nullable<uint8_t> currentLevel;

	Clusters::LevelControl::Attributes::CurrentLevel::Get(kLightEndpointId, currentLevel);
	if (!currentLevel.IsNull()) {
		current = currentLevel.Value();
	}

	int ret = Instance().mPWMDevice.Init(&sLightPwmDevice, minLightLevel, maxLightLevel, maxLightLevel);
	if (ret != 0) {
		return chip::System::MapErrorZephyr(ret);
	}
	// Register PWM device init and activate callback's
	Instance().mPWMDevice.SetCallbacks(ActionInitiated, ActionCompleted);
	/* SET a stored value */
	if (current) {
		AppTask::Instance().GetPWMDevice().InitiateAction(
			PWMDevice::LEVEL_ACTION,
			static_cast<int32_t>(AppEventType::Lighting),
			reinterpret_cast<uint8_t *>(&current));
	}

	// Init modified persistent storage setup
	gExampleDeviceInfoProvider.SetStorageDelegate(
		&Server::GetInstance().GetPersistentStorage());
	chip::DeviceLayer::SetDeviceInfoProvider(&gExampleDeviceInfoProvider);
	app::SetAttributePersistenceProvider(&gDeferredAttributePersister);
	MatterUi::Instance().Init(AppTask::ButtonUpdateHandler);

	return CHIP_NO_ERROR;
}

void AppTask::ButtonUpdateHandler(uint32_t button_state, uint32_t has_changed)
{
	if (has_changed & 1) {
		if (button_state & 1) {
			LOG_INF("Factoryreset button pressed");
		} else {
			LOG_INF("Factoryreset button released");
		}
		MatterUi::Instance().AppFactoryResetEventTrig();
	}
}

void AppTask::PostEvent(AppEvent *aEvent)
{
	if (!aEvent) {
		return;
	}

	if (k_msgq_put(&sAppEventQueue, aEvent, K_NO_WAIT) != 0) {
		LOG_INF("PostEvent fail");
	}
}

void AppTask::DispatchEvent(const AppEvent *aEvent)
{
	if (!aEvent) {
		return;
	}
	if (aEvent->Handler) {
		aEvent->Handler(aEvent);
	} else {
		LOG_INF("Dropping event without handler");
	}
}

void AppTask::GetEvent(AppEvent *aEvent)
{
	k_msgq_get(&sAppEventQueue, aEvent, K_FOREVER);
}

void AppTask::IdentifyStartHandler(Identify *)
{
	AppEvent event;
	event.Type = AppEventType::IdentifyStart;
	event.Handler = [](const AppEvent *) {
		MatterStack::Instance().IdentifyLedState(true);
		LOG_INF("Identify start");
	};
	PostEvent(&event);
}

void AppTask::IdentifyStopHandler(Identify *)
{
	AppEvent event;
	event.Type = AppEventType::IdentifyStop;
	event.Handler = [](const AppEvent *) {
		LOG_INF("Identify stop");
		MatterStack::Instance().IdentifyLedState(false);
	};
	PostEvent(&event);
}

void AppTask::LightingActionEventHandler(const AppEvent *event)
{
	if (event->Type != AppEventType::Lighting) {
		return;
	}

	PWMDevice::Action_t action = static_cast<PWMDevice::Action_t>(event->LightingEvent.Action);
	int32_t actor = event->LightingEvent.Actor;

	LOG_INF("Light state to %d by %d", action, actor);

	if (Instance().mPWMDevice.InitiateAction(action, actor, NULL)) {
		LOG_INF("Action is already in progress or active.");
	}
}

void AppTask::StartBLEAdvertisementHandler(const AppEvent *)
{
	if (Server::GetInstance().GetFabricTable().FabricCount() != 0) {
		LOG_INF("Matter service BLE advertising not started - device is already "
			"commissioned");
		return;
	}

	if (ConnectivityMgr().IsBLEAdvertisingEnabled()) {
		LOG_INF("BLE advertising is already enabled");
		return;
	}

	if (Server::GetInstance().GetCommissioningWindowManager().OpenBasicCommissioningWindow() !=
	    CHIP_NO_ERROR) {
		LOG_ERR("OpenBasicCommissioningWindow() failed");
	}
}

void AppTask::ActionInitiated(PWMDevice::Action_t action, int32_t actor)
{
	if (action == PWMDevice::ON_ACTION) {
		LOG_INF("Turn On Action has been initiated");
	} else if (action == PWMDevice::OFF_ACTION) {
		LOG_INF("Turn Off Action has been initiated");
	} else if (action == PWMDevice::LEVEL_ACTION) {
		LOG_INF("Level Action has been initiated");
	}
	MatterStack::Instance().StatusLedBlink();
}

void AppTask::ActionCompleted(PWMDevice::Action_t action, int32_t actor)
{
	if (action == PWMDevice::ON_ACTION) {
		LOG_INF("Turn On Action has been completed");
	} else if (action == PWMDevice::OFF_ACTION) {
		LOG_INF("Turn Off Action has been completed");
	} else if (action == PWMDevice::LEVEL_ACTION) {
		LOG_INF("Level Action has been completed");
	}

	if (actor == static_cast<int32_t>(AppEventType::ShellButton)) {
		Instance().UpdateClusterState();
	}
}

void AppTask::UpdateClusterState()
{
	SystemLayer().ScheduleLambda([this] {
		// write the new on/off value
		Protocols::InteractionModel::Status status =
			Clusters::OnOff::Attributes::OnOff::Set(kLightEndpointId,
								mPWMDevice.IsTurnedOn());

		if (status != Protocols::InteractionModel::Status::Success) {
			LOG_ERR("Updating on/off cluster failed: %x", to_underlying(status));
		}

		// write the current level
		status = Clusters::LevelControl::Attributes::CurrentLevel::Set(
			kLightEndpointId, mPWMDevice.GetLevel());

		if (status != Protocols::InteractionModel::Status::Success) {
			LOG_ERR("Updating level cluster failed: %x", to_underlying(status));
		}
	});
}

CHIP_ERROR AppTask::Init()
{
	/* Initialize Matter stack */
	ReturnErrorOnFailure(MatterStack::Instance().matter_stack_init(DevInit));

	chip::app::InteractionModelEngine::GetInstance()->RegisterReadHandlerAppCallback(
		&ICDHandler::Instance());

	/* Start Matter sheduler */
	ReturnErrorOnFailure(MatterStack::Instance().matter_stack_start());

	return CHIP_NO_ERROR;
}

CHIP_ERROR AppTask::StartApp()
{
	ReturnErrorOnFailure(Init());

	AppEvent event = {};

	while (true) {
		GetEvent(&event);
		DispatchEvent(&event);
	}

	return CHIP_NO_ERROR;
}
