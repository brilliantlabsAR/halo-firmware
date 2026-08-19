/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include "ShellCommands.h"
#include "MatterStack.h"
#include "LightSwitch.h"
#include <platform/CHIPDeviceLayer.h>

#include "BindingHandler.h"

using namespace chip;
using namespace chip::app;

namespace SwitchCommands
{
using Shell::Engine;
using Shell::shell_command_t;
using Shell::streamer_get;
using Shell::streamer_printf;

static CHIP_ERROR LightComandHandler(int argc, char **argv)
{
	if (argc != 1) {
		streamer_printf(SwitchCommands::streamer_get(), "Usage: light [on|off|toggle]");
	}

	if (strcmp(argv[0], "on") == 0) {
		LightSwitch::GetInstance().LightControl(LightSwitch::Action::On);
	} else if (strcmp(argv[0], "off") == 0) {
		LightSwitch::GetInstance().LightControl(LightSwitch::Action::Off);
	} else if (strcmp(argv[0], "toggle") == 0) {
		LightSwitch::GetInstance().LightControl(LightSwitch::Action::Toggle);
	} else {
		streamer_printf(SwitchCommands::streamer_get(), "Usage: light [on|off|toggle]");
	}

	return CHIP_NO_ERROR;
}

static CHIP_ERROR BindTablePrint(int argc, char **argv)
{
	BindingHandler::GetInstance().PrintTable();
	MatterStack::Instance().matter_stack_fabric_print();
	return CHIP_NO_ERROR;
}

void RegisterSwitchCommands(void)
{
	static const shell_command_t sLightCommand = {
		LightComandHandler, "light", "Light test commands. Usage: light [on|off|toggle]"};
	static const shell_command_t sTableCommand = {BindTablePrint, "table",
						      "Print light binding table and Fabric table user data. Usage: table"};

	Engine::Root().RegisterCommands(&sLightCommand, 1);
	Engine::Root().RegisterCommands(&sTableCommand, 1);
	return;
}

} // namespace SwitchCommands
