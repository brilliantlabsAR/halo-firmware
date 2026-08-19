/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include "AppTask.h"
#include "PWMDevice.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/ConcreteAttributePath.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace chip;
using namespace chip::app::Clusters;
using namespace chip::app::Clusters::OnOff;

void MatterPostAttributeChangeCallback(const chip::app::ConcreteAttributePath &attributePath,
				       uint8_t type, uint16_t size, uint8_t *value)
{
	ClusterId clusterId = attributePath.mClusterId;
	AttributeId attributeId = attributePath.mAttributeId;

	if (clusterId == OnOff::Id && attributeId == OnOff::Attributes::OnOff::Id) {
		LOG_INF("Cluster OnOff: attribute OnOff set to %u", *value);
		AppTask::Instance().GetPWMDevice().InitiateAction(
			*value ? PWMDevice::ON_ACTION : PWMDevice::OFF_ACTION,
			static_cast<int32_t>(AppEventType::Lighting), value);
	} else if (clusterId == LevelControl::Id &&
		   attributeId == LevelControl::Attributes::CurrentLevel::Id) {
		LOG_INF("Cluster LevelControl: attribute CurrentLevel set to %u", *value);
		if (AppTask::Instance().GetPWMDevice().IsTurnedOn()) {
			AppTask::Instance().GetPWMDevice().InitiateAction(
				PWMDevice::LEVEL_ACTION,
				static_cast<int32_t>(AppEventType::Lighting), value);
		} else {
			LOG_INF("LED is off. Try to use move-to-level-with-on-off "
				"instead of move-to-level");
		}
	}
}

void emberAfOnOffClusterInitCallback(EndpointId endpoint)
{
	/* Device init will handle Cluster  */
}
