/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#pragma once

#include "AppEvent.h"
#include "PWMDevice.h"

#include <platform/CHIPDeviceLayer.h>
#include <platform/alif/DeviceInstanceInfoProviderImpl.h>

#include <cstdint>

struct Identify;

class AppTask
{
public:
    static AppTask & Instance()
    {
        static AppTask sAppTask;
        return sAppTask;
    };

    CHIP_ERROR StartApp();
    void UpdateClusterState();
    PWMDevice & GetPWMDevice() { return mPWMDevice; }

    static void IdentifyStartHandler(Identify *);
    static void IdentifyStopHandler(Identify *);
    static void ButtonUpdateHandler(uint32_t button_state, uint32_t has_changed);

private:
    CHIP_ERROR Init();
    static CHIP_ERROR DevInit();

    static void LightingActionEventHandler(const AppEvent * event);
    static void PostEvent(AppEvent * event);
    static void DispatchEvent(const AppEvent * event);
    static void GetEvent(AppEvent * aEvent);
    static void StartBLEAdvertisementHandler(const AppEvent * event);
    static void ActionInitiated(PWMDevice::Action_t action, int32_t actor);
    static void ActionCompleted(PWMDevice::Action_t action, int32_t actor);

    PWMDevice mPWMDevice;
};
