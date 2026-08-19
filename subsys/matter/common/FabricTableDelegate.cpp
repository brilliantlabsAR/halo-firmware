/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include "FabricTableDelegate.h"

AppFabricTableDelegate::~AppFabricTableDelegate()
{
	chip::Server::GetInstance().GetFabricTable().RemoveFabricDelegate(this);
}

void AppFabricTableDelegate::Init()
{
	static AppFabricTableDelegate sAppFabricDelegate;

	chip::Server::GetInstance().GetFabricTable().AddFabricDelegate(&sAppFabricDelegate);
	k_timer_init(&sFabricRemovedTimer, &OnFabricRemovedTimerCallback, nullptr);
}

void AppFabricTableDelegate::OnFabricRemoved(const chip::FabricTable &fabricTable,
					     chip::FabricIndex fabricIndex)
{
	k_timer_start(&sFabricRemovedTimer, K_MSEC(1000), K_NO_WAIT);
}

void AppFabricTableDelegate::OnFabricRemovedTimerCallback(k_timer *timer)
{
	if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
		chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) {
			// Erase PersistedStorage data and Network credentials
			chip::DeviceLayer::PersistedStorage::KeyValueStoreMgrImpl()
				.DoFactoryReset();
			chip::DeviceLayer::ConnectivityMgr().ErasePersistentInfo();
			// Activate BLE advertisment
			if (!chip::DeviceLayer::ConnectivityMgr().IsBLEAdvertisingEnabled()) {
				if (CHIP_NO_ERROR == chip::Server::GetInstance()
							     .GetCommissioningWindowManager()
							     .OpenBasicCommissioningWindow()) {
					return;
				}
			}
			ChipLogError(FabricProvisioning, "Could not start BLE advertising");
		});
	}
}
