/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include "PWMDevice.h"

#include "AppConfig.h"

#include <lib/support/CodeUtils.h>

#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

int PWMDevice::Init(const pwm_dt_spec * aPWMDevice, uint8_t aMinLevel, uint8_t aMaxLevel, uint8_t aDefaultLevel)
{
	mState = kState_On;
	mMinLevel = aMinLevel;
	mMaxLevel = aMaxLevel;
	mLevel = aDefaultLevel;
	mPwmDevice = aPWMDevice;

    if (!mPwmDevice) {
        LOG_ERR("NO Driver");
        return -ENODEV;
    }

	if (!device_is_ready(mPwmDevice->dev)) {
		LOG_ERR("PWM device %s is not ready", mPwmDevice->dev->name);
		return -ENODEV;
	}

    /* Disable Light state at init*/
    SetLevel(0);
    Set(false);
    return 0;
}

void PWMDevice::SetCallbacks(PWMCallback aActionInitiatedClb, PWMCallback aActionCompletedClb)
{
    mActionInitiatedClb = aActionInitiatedClb;
    mActionCompletedClb = aActionCompletedClb;
}

bool PWMDevice::InitiateAction(Action_t aAction, int32_t aActor, uint8_t * aValue)
{
    bool action_initiated = false;
    State_t new_state;

    // Initiate On/Off Action only when the previous one is complete.
    if (mState == kState_Off && aAction == ON_ACTION) {
        action_initiated = true;
        new_state        = kState_On;
    } else if (mState == kState_On && aAction == OFF_ACTION) {
        action_initiated = true;
        new_state        = kState_Off;
    } else if (aAction == LEVEL_ACTION && *aValue != mLevel) {
        action_initiated = true;
        if (*aValue == 0) {
            new_state = kState_Off;
        } else {
            new_state = kState_On;
        }
    }

    if (action_initiated) {
        if (mActionInitiatedClb) {
            mActionInitiatedClb(aAction, aActor);
        }

        if (aAction == ON_ACTION || aAction == OFF_ACTION) {
            Set(new_state == kState_On);
        } else if (aAction == LEVEL_ACTION) {
            mState = new_state;
            SetLevel(*aValue);
        }

        if (mActionCompletedClb) {
            mActionCompletedClb(aAction, aActor);
        }
    }

    return action_initiated;
}

void PWMDevice::SetLevel(uint8_t aLevel)
{
    LOG_INF("Setting brightness level to %u", aLevel);
    mLevel = aLevel;
    ApplyLevel();
}

void PWMDevice::Set(bool aOn)
{
    mState = aOn ? kState_On : kState_Off;
    ApplyLevel();
}

void PWMDevice::SuppressOutput()
{
    pwm_set_pulse_dt(mPwmDevice, 0);
    
}

void PWMDevice::ApplyLevel()
{
	const uint8_t maxEffectiveLevel = mMaxLevel - mMinLevel;
	const uint8_t effectiveLevel =
		mState == kState_On ? chip::min<uint8_t>(mLevel - mMinLevel, maxEffectiveLevel) : 0;

	pwm_set_pulse_dt(mPwmDevice,
			 static_cast<uint32_t>(static_cast<const uint64_t>(mPwmDevice->period) *
					       effectiveLevel / maxEffectiveLevel));
}
