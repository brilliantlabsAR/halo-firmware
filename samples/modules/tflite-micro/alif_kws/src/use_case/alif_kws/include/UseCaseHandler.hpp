/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 *
 */

/*
 * Copyright (c) 2021 Arm Limited. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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
#ifndef ALIF_KWS_EVT_HANDLER_HPP
#define ALIF_KWS_EVT_HANDLER_HPP

#include "AppContext.hpp"

namespace alif {
namespace app {

    /**
     * @brief       Handles the inference event.
     * @param[in]   ctx         Pointer to the application context.
     * @return      true or false based on execution success.
     **/
    bool ClassifyAudioHandler(arm::app::ApplicationContext& ctx, bool oneshot);

} /* namespace app */
} /* namespace alif */

#endif /* ALIF_KWS_EVT_HANDLER_HPP */
