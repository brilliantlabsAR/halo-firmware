/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#pragma once

#include <cstdint>

struct AppEvent;
using EventHandler = void (*)(const AppEvent *);

enum class AppEventType : uint8_t {
	None = 0,
	IdentifyStart,
	IdentifyStop,
};

struct AppEvent {
	AppEventType Type{AppEventType::None};
	EventHandler Handler;
};
