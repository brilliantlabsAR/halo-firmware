/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#pragma once
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <platform/CHIPDeviceLayer.h>

#include <credentials/examples/DeviceAttestationCredsExample.h>

#if CONFIG_CHIP_FACTORY_DATA
#include <platform/alif/FactoryDataProvider.h>
#else
#include <platform/alif/DeviceInstanceInfoProviderImpl.h>
#endif

#include <cstdint>

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::Credentials;
using namespace ::chip::DeviceLayer;

struct CommissioningFabricTable {
	uint64_t nodeId;
	FabricIndex fabricIndex;
	bool in_use;
	bool commission;
};

#define MATTER_FABRIC_TABLE_MAX_SIZE 2

class MatterStack
{
      private:
	using DevInit = CHIP_ERROR (*)(void);

	MatterStack()
	{
		k_mutex_init(&sInitMutex);
		k_condvar_init(&sInitCondVar);
		ready = false;
		sIsThreadProvisioned = false;
		sIsThreadEnabled = false;
		sIsThreadAttached = false;
		sHaveBLEConnections = false;
		sHaveCommission = false;
		sHaveSubcribed = false;
		sEndpointSubsripted =false;
		sBlinkLed = false;
		sIdentifyLed = false;
		sLedStatusPeriod = 500;
		for (int i=0; i< MATTER_FABRIC_TABLE_MAX_SIZE;i++) {
			fabricTable[i].in_use = false;
			fabricTable[i].commission = false;
		}
	}
	struct k_mutex sInitMutex;
	struct k_condvar sInitCondVar;
	struct CommissioningFabricTable fabricTable[MATTER_FABRIC_TABLE_MAX_SIZE];
	bool ready;
	bool sIsThreadProvisioned;
	bool sIsThreadEnabled;
	bool sIsThreadAttached;
	bool sHaveBLEConnections;
	bool sHaveSubcribed;
	bool sEndpointSubsripted;
	bool sHaveCommission;
	bool sBlinkLed;
	bool sIdentifyLed;
	CHIP_ERROR sInitResult;
	int sLedStatusPeriod;

#if CONFIG_CHIP_FACTORY_DATA
	chip::DeviceLayer::FactoryDataProvider<chip::DeviceLayer::ExternalFlashFactoryData>
		mFactoryDataProvider;
#endif
	DevInit dev_init_cb{nullptr};

	void wait_condition()
	{
		k_mutex_lock(&sInitMutex, K_FOREVER);
		if (!ready) {
			k_condvar_wait(&sInitCondVar, &sInitMutex, K_FOREVER);
		}
		k_mutex_unlock(&sInitMutex);
	}

	void signal_condition()
	{
		ready = true;
		k_condvar_signal(&sInitCondVar);
		k_mutex_unlock(&sInitMutex);
	}

	void matter_internal_init();

	// Need to be used static because these functions are registered to Matter scheduler
	static void InitInternal(intptr_t class_ptr);
	static void MatterStateEventHandler(intptr_t aArg);
	static void ChipEventHandler(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t arg);
	static void LedStatusUpdate(intptr_t class_ptr);
	static void matter_stack_fabric_add(const chip::DeviceLayer::ChipDeviceEvent *event, bool commission_fabric);
	static void matter_stack_fabric_remove(FabricIndex fabricIndex);

      public:
	CHIP_ERROR matter_stack_init(DevInit device_init_cb);
	CHIP_ERROR matter_stack_start();
	void matter_stack_fabric_print();
	void StatusLedBlink();
	void IdentifyLedState(bool enable);
	void MatterEndpointSubscripted();
	void MatterStateMachineEventTrig(void);

	static MatterStack &Instance()
	{
		static MatterStack sMatterStack;
		return sMatterStack;
	};
};
