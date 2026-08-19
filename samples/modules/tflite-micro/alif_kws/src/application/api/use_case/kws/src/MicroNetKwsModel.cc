/*
 * SPDX-FileCopyrightText: Copyright 2021 Arm Limited and/or its affiliates
 * <open-source-office@arm.com> SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "MicroNetKwsModel.hpp"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(MicroNetKwsModel);

const tflite::MicroOpResolver &arm::app::MicroNetKwsModel::GetOpResolver()
{
	return this->m_opResolver;
}

bool arm::app::MicroNetKwsModel::EnlistOperations()
{
#ifndef ETHOS_U_NPU_ASSUMED
	this->m_opResolver.AddReshape();
	this->m_opResolver.AddAveragePool2D();
	this->m_opResolver.AddConv2D();
	this->m_opResolver.AddDepthwiseConv2D();
	this->m_opResolver.AddFullyConnected();
	this->m_opResolver.AddRelu();
#endif

	if (kTfLiteOk == this->m_opResolver.AddEthosU()) {
		LOG_INF("Added %s support to op resolver", tflite::GetString_ETHOSU());
	} else {
		LOG_ERR("Failed to add Arm NPU support to op resolver.");
		return false;
	}
	return true;
}
