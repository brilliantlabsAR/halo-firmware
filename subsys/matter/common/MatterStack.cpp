/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include "MatterStack.h"
#include "AppTask.h"
#include "MatterUi.h"
#include "BLEManagerImpl.h"
#include "FabricTableDelegate.h"

#include <DeviceInfoProviderImpl.h>
#include <app/TestEventTriggerDelegate.h>
#include <app/clusters/identify-server/identify-server.h>
#include <app/clusters/ota-requestor/OTATestEventTriggerHandler.h>
#include <app/server/OnboardingCodesUtil.h>
#include <app/server/Server.h>

#include <zephyr/fs/nvs.h>
#include <zephyr/settings/settings.h>

using namespace ::chip;
using namespace chip::app;
using namespace chip::Credentials;
using namespace ::chip::DeviceLayer;

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

namespace
{
// Test network key
uint8_t sTestEventTriggerEnableKey[TestEventTriggerDelegate::kEnableKeyLength] = {
	0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
	0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
#define VerifyInitResultOrReturn(ec, msg)                                                          \
	VerifyOrReturn(ec == CHIP_NO_ERROR,                                                        \
		       LOG_ERR(msg " [Error: %d]", Instance().sInitResult.Format()))
} // namespace

void MatterStack::matter_internal_init()
{
	Instance().sInitResult = ThreadStackMgr().InitThreadStack();
	VerifyInitResultOrReturn(Instance().sInitResult,
				 "ThreadStackMgr().InitThreadStack() failed");

#if CONFIG_CHIP_THREAD_SSED
	Instance().sInitResult = ConnectivityMgr().SetThreadDeviceType(
		ConnectivityManager::kThreadDeviceType_SynchronizedSleepyEndDevice);
#elif CONFIG_OPENTHREAD_MTD_SED
	Instance().sInitResult = ConnectivityMgr().SetThreadDeviceType(
		ConnectivityManager::kThreadDeviceType_SleepyEndDevice);
#else
	Instance().sInitResult = ConnectivityMgr().SetThreadDeviceType(
		ConnectivityManager::kThreadDeviceType_Router);
#endif
	VerifyInitResultOrReturn(Instance().sInitResult, "SetThreadDeviceType fail");

#if CONFIG_CHIP_FACTORY_DATA
	Instance().sInitResult = mFactoryDataProvider.Init();
	VerifyInitResultOrReturn(Instance().sInitResult, "FactoryDataProvider::Init() failed");
	SetDeviceInstanceInfoProvider(&mFactoryDataProvider);
	SetDeviceAttestationCredentialsProvider(&mFactoryDataProvider);
	SetCommissionableDataProvider(&mFactoryDataProvider);
	// Read EnableKey from the factory data.
	MutableByteSpan enableKey(sTestEventTriggerEnableKey);
	CHIP_ERROR err = mFactoryDataProvider.GetEnableKey(enableKey);
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("mFactoryDataProvider.GetEnableKey() failed. Could not delegate a test "
			"event trigger");
		memset(sTestEventTriggerEnableKey, 0, sizeof(sTestEventTriggerEnableKey));
	}
#else
	SetDeviceInstanceInfoProvider(&DeviceInstanceInfoProviderMgrImpl());
	SetDeviceAttestationCredentialsProvider(Examples::GetExampleDACProvider());
#endif

	// Init ZCL Data Model and start server
	static CommonCaseDeviceServerInitParams initParams;
	static SimpleTestEventTriggerDelegate sTestEventTriggerDelegate{};
	static OTATestEventTriggerHandler sOtaTestEventTriggerHandler{};
	Instance().sInitResult =
		sTestEventTriggerDelegate.Init(ByteSpan(sTestEventTriggerEnableKey));
	VerifyInitResultOrReturn(Instance().sInitResult, "Tesat trigger delegate init fail");
	Instance().sInitResult = sTestEventTriggerDelegate.AddHandler(&sOtaTestEventTriggerHandler);
	VerifyInitResultOrReturn(Instance().sInitResult,
				 "OTa test event trigger handlertrigger addfail");
	(void)initParams.InitializeStaticResourcesBeforeServerInit();
	initParams.testEventTriggerDelegate = &sTestEventTriggerDelegate;
	Instance().sInitResult = chip::Server::GetInstance().Init(initParams);
	VerifyInitResultOrReturn(Instance().sInitResult, "Server init fail");
	AppFabricTableDelegate::Init();

	ConfigurationMgr().LogDeviceConfig();
	PrintOnboardingCodes(
		chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));

	PlatformMgr().AddEventHandler(Instance().ChipEventHandler, reinterpret_cast<intptr_t>(this));

	if (Instance().dev_init_cb) {
		Instance().sInitResult = Instance().dev_init_cb();
		VerifyInitResultOrReturn(Instance().sInitResult, "Device post init fail");
	}

	MatterStateMachineEventTrig();	
}

void MatterStack::InitInternal(intptr_t class_ptr)
{

	MatterStack *entry = reinterpret_cast<MatterStack *>(class_ptr);

	entry->matter_internal_init();
	entry->signal_condition();
}

void MatterStack::LedStatusUpdate(intptr_t class_ptr)
{
	MatterStack *entry = reinterpret_cast<MatterStack *>(class_ptr);
	int led_period = 0;
	bool ble_led = false;
	bool single_event = false;

	if (entry->sIsThreadProvisioned) {
		if (!entry->sIsThreadAttached || !entry->sHaveSubcribed ||
		    entry->sHaveBLEConnections) {
			led_period = 100;
		} else if (entry->sHaveSubcribed && !entry->sEndpointSubsripted) {
			entry->sEndpointSubsripted = true;
			led_period = 2000;
			single_event = true;
		} else if (entry->sIdentifyLed) {
			led_period = 1000;
		}
		ble_led = entry->sHaveBLEConnections;
	} else {
		ble_led = true;
		if (entry->sHaveBLEConnections) {
			led_period = 100;
		} else {
			led_period = 500;
		}
	}
	/* Update Boot period or do short Led Blink Indication */
	entry->sLedStatusPeriod = led_period;

	/* Clear a blink flag */
	entry->sBlinkLed = false;

	MatterUi::Instance().StatusLedTimerStart(led_period, ble_led, single_event);
}

void MatterStack::ChipEventHandler(const ChipDeviceEvent *event, intptr_t arg)
{
	MatterStack *entry = reinterpret_cast<MatterStack *>(arg);

	switch (event->Type) {
	case DeviceEventType::kCHIPoBLEAdvertisingChange:
		entry->sHaveBLEConnections = ConnectivityMgr().NumBLEConnections() != 0;
		LOG_INF("BLE connection state %d", Instance().sHaveBLEConnections);
		break;
	case DeviceEventType::kOperationalNetworkEnabled:
		LOG_INF("Network Enabled");
		break;

	case DeviceEventType::kDnssdInitialized:
		LOG_INF("DNS init done");
		break;

	case DeviceEventType::kDnssdRestartNeeded:
		LOG_INF("DNS Restart needed");
		break;

	case DeviceEventType::kThreadConnectivityChange:
		LOG_INF("Thread connectivity change: %d", event->ThreadConnectivityChange.Result);
		if (event->ThreadConnectivityChange.Result == kConnectivity_Established) {
			LOG_INF("Thread connectivity established");
		} else if (event->ThreadConnectivityChange.Result == kConnectivity_Lost) {
			LOG_INF("Thread connectivity lost");
		}
		break;

	case DeviceEventType::kThreadStateChange:
		entry->sIsThreadProvisioned = ConnectivityMgr().IsThreadProvisioned();
		entry->sIsThreadEnabled = ConnectivityMgr().IsThreadEnabled();
		entry->sIsThreadAttached = ConnectivityMgr().IsThreadAttached();
		LOG_INF("Thread State Provisioned %d, enabled %d, Atteched %d",
			entry->sIsThreadProvisioned, entry->sIsThreadEnabled,
			entry->sIsThreadAttached);
		if (!entry->sIsThreadAttached) {
			entry->sHaveSubcribed = false;
			entry->sEndpointSubsripted = false;
		}
		break;

	case DeviceEventType::kCommissioningComplete:
		matter_stack_fabric_add(event, entry->sHaveCommission);
		if (entry->sHaveCommission) {
			LOG_INF("Commission complete node ide %lld, fabric %d",
				event->CommissioningComplete.nodeId,
				event->CommissioningComplete.fabricIndex);
			entry->sHaveCommission = false;
		} else {
			LOG_INF("Commission complete for data flow node ide %lld, fabric %d",
				event->CommissioningComplete.nodeId,
				event->CommissioningComplete.fabricIndex);
		}

		break;

	case DeviceEventType::kServiceProvisioningChange:
		LOG_INF("Service Provisioned %d, conf update %d",
			event->ServiceProvisioningChange.IsServiceProvisioned,
			event->ServiceProvisioningChange.ServiceConfigUpdated);
		break;
	case DeviceEventType::kFailSafeTimerExpired:
		LOG_INF("Commission fail safe timer expiration: fab id %d, NoCinvoked %d, "
			"updNoCCmd %d",
			event->FailSafeTimerExpired.fabricIndex,
			event->FailSafeTimerExpired.addNocCommandHasBeenInvoked,
			event->FailSafeTimerExpired.updateNocCommandHasBeenInvoked);
			matter_stack_fabric_remove(event->FailSafeTimerExpired.fabricIndex);
		break;
	case DeviceEventType::kCHIPoBLEConnectionEstablished:
		LOG_INF("BLE connection establishment");
		entry->sHaveCommission = true;
	break;
	case DeviceEventType::kCHIPoBLEConnectionClosed:
		LOG_INF("BLE connection closed");
		entry->sHaveBLEConnections = ConnectivityMgr().NumBLEConnections() != 0;
		entry->sHaveCommission = false;
	break;
	case DeviceEventType::kServerReady:
		LOG_INF("Server Init Complete");
	break;

	default:
		LOG_INF("Unhandled event types: %d", event->Type);
		break;
	}

	// Set Status led indication
	entry->MatterStateMachineEventTrig();
}

void MatterStack::MatterStateEventHandler(intptr_t aArg)
{
	MatterStack *entry = reinterpret_cast<MatterStack *>(aArg);
	entry->LedStatusUpdate(aArg);
}

void MatterStack::MatterStateMachineEventTrig(void)
{
	PlatformMgr().ScheduleWork(MatterStateEventHandler, reinterpret_cast<intptr_t>(this));
}

CHIP_ERROR MatterStack::matter_stack_init(DevInit device_init_cb)
{
	CHIP_ERROR err = CHIP_NO_ERROR;
	k_mutex_lock(&Instance().sInitMutex, K_FOREVER);
	dev_init_cb = device_init_cb;

	err = chip::Platform::MemoryInit();
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("MemoryInit fail");
		return err;
	}

	err = PlatformMgr().InitChipStack();
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("InitChipStack fail");
		return err;
	}
	// Shedule Init
	return PlatformMgr().ScheduleWork(InitInternal, reinterpret_cast<intptr_t>(this));
}

CHIP_ERROR MatterStack::matter_stack_start()
{
	CHIP_ERROR err = PlatformMgr().StartEventLoopTask();
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("PlatformMgr().StartEventLoopTask() failed");
		return err;
	}
	wait_condition();
	return sInitResult;
}

void MatterStack::matter_stack_fabric_add(const chip::DeviceLayer::ChipDeviceEvent *event,
					  bool commission_fabric)
{
	struct CommissioningFabricTable *entry = NULL;

	for (int i = 0; i < MATTER_FABRIC_TABLE_MAX_SIZE; i++) {
		if (Instance().fabricTable[i].in_use &&

		    Instance().fabricTable[i].fabricIndex ==
			    event->CommissioningComplete.fabricIndex) {
			entry = &Instance().fabricTable[i];
			break;
		}
	}

	if (!entry) {
		for (int i = 0; i < MATTER_FABRIC_TABLE_MAX_SIZE; i++) {
			if (!Instance().fabricTable[i].in_use) {
				entry = &Instance().fabricTable[i];
				break;
			}
		}
	}

	if (entry) {
		entry->in_use = true;
		entry->commission = commission_fabric;
		entry->fabricIndex = event->CommissioningComplete.fabricIndex;
		entry->nodeId = event->CommissioningComplete.nodeId;
	}
}

void MatterStack::matter_stack_fabric_remove(FabricIndex fabricIndex)
{
	struct CommissioningFabricTable *entry = NULL;

	for (int i=0; i < MATTER_FABRIC_TABLE_MAX_SIZE; i++) {
		if (!Instance().fabricTable[i].in_use || Instance().fabricTable[i].fabricIndex != fabricIndex) {
			continue;
		}
		entry = &Instance().fabricTable[i];
		break;
	}
	
	if (entry) {
		entry->in_use = false;
		entry->fabricIndex = 0;
		entry->nodeId = 0;
	}
}

void MatterStack::matter_stack_fabric_print()
{
	for (int i = 0; i < MATTER_FABRIC_TABLE_MAX_SIZE; i++) {
		if (!fabricTable[i].in_use) {
			continue;
		}
		LOG_INF("Fabric session complete node ide %lld, fabric %d admin session %u",
			fabricTable[i].nodeId, fabricTable[i].fabricIndex,
			fabricTable[i].commission);
	}
}

void MatterStack::StatusLedBlink()
{
	if (Instance().sLedStatusPeriod == 0 && !Instance().sBlinkLed) {
		Instance().sBlinkLed = true;
		MatterStateMachineEventTrig();
	}
}

void MatterStack::IdentifyLedState(bool enable)
{
	if (Instance().sIdentifyLed != enable) {
		Instance().sIdentifyLed = enable;
		Instance().MatterStateMachineEventTrig();
	}
}

void MatterStack::MatterEndpointSubscripted()
{
	if (!Instance().sHaveSubcribed) {
		Instance().sHaveSubcribed = true;
		Instance().MatterStateMachineEventTrig();
	} else {
		Instance().StatusLedBlink();
	}
	
}
