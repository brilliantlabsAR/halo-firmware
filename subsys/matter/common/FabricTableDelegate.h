/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/kernel.h>
#include <platform/CHIPDeviceLayer.h>
#include <app/server/Server.h>

class AppFabricTableDelegate : public chip::FabricTable::Delegate
{
      public:
	virtual ~AppFabricTableDelegate();

	/**
	 * @brief Initialize module and add a delegation to the Fabric Table.
	 *
	 * To use the OnFabricRemoved method defined within this class and allow to react on the
	 * last fabric removal this method should be called in the application code.
	 */
	static void Init();

      private:
	void OnFabricRemoved(const chip::FabricTable &fabricTable, chip::FabricIndex fabricIndex);
	static void OnFabricRemovedTimerCallback(k_timer *timer);

	inline static k_timer sFabricRemovedTimer;
};
