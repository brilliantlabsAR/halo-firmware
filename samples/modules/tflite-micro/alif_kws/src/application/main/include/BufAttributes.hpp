/*
 * SPDX-FileCopyrightText: Copyright 2021 Arm Limited and/or its affiliates <open-source-office@arm.com>
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

#ifndef BUF_ATTRIBUTES_HPP
#define BUF_ATTRIBUTES_HPP

#include <zephyr/devicetree.h>

#define BYTE_ALIGNMENT              16
#define ALIGNMENT_REQ               aligned(BYTE_ALIGNMENT)
#define ACTIVATION_BUF_SECTION      section(CONFIG_ACTIVATION_BUF_SECTION)

#if CONFIG_MODEL_IN_EXT_FLASH
#define MODEL_SECTION               section(".alif_extflash_" DT_PROP(DT_NODELABEL(ext_flash_xip), zephyr_memory_region))
#else
#define MODEL_SECTION               section(CONFIG_MODEL_SECTION)
#endif

#define MAKE_ATTRIBUTE(x)           __attribute__((ALIGNMENT_REQ, x))
#define MODEL_TFLITE_ATTRIBUTE      MAKE_ATTRIBUTE(MODEL_SECTION)
#define ACTIVATION_BUF_ATTRIBUTE    MAKE_ATTRIBUTE(ACTIVATION_BUF_SECTION)
#define LABELS_ATTRIBUTE


#endif /* BUF_ATTRIBUTES_HPP */
