/*
 * SPDX-FileCopyrightText: Copyright 2021-2024 Arm Limited and/or its
 * affiliates <open-source-office@arm.com>
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


#include "TensorFlowLiteMicro.hpp" /* our inference logic api */

#include <cstdio>
#include <new>
#include <exception>

#include <zephyr/console/console.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(Main);

extern void main_loop();

#if defined(__ARMCC_VERSION) && (__ARMCC_VERSION >= 6010050)
__ASM(" .global __ARM_use_no_argv\n");
#endif

/* Print application information. */
static void print_application_intro()
{
	LOG_INF("Build date: " __DATE__ " @ " __TIME__ "");
	LOG_INF("Copyright 2021-2024 Arm Limited and/or its affiliates "
	     "<open-source-office@arm.com>");
}

static void out_of_heap()
{
	LOG_WRN("Out of heap");
	std::terminate();
}

int main()
{
	console_getline_init();

	print_application_intro();

	/* Check the version of TensorFlow Lite Micro. */
	PrintTensorFlowVersion();

	std::set_new_handler(out_of_heap);

	/* Run the application. */
	main_loop();

	/* This is unreachable without errors. */
	LOG_INF("program terminating...");

	return 0;
}
