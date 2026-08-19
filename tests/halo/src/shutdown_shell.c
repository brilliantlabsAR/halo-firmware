#include <string.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/shell/shell.h>
#include <zephyr/drivers/sm/sm.h>

const struct device *sm = NULL;

static int cmd_shutdown(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	sm = DEVICE_DT_GET(DT_ALIAS(shutdown));

	if (!device_is_ready(sm)) {
		shell_error(sh, "sm device %s not ready", sm->name);
		return -1;
	}

	shell_print(sh, "Shutting down - entering ship mode");

	shutdown(sm);

	return err;
}

static int cmd_reboot(const struct shell *sh, size_t argc, char *argv[])
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Rebooting system...");
	sys_reboot(SYS_REBOOT_COLD);

	return 0;
}

SHELL_CMD_REGISTER(reboot, NULL, "reboot", cmd_reboot);
SHELL_CMD_REGISTER(shutdown, NULL, "shutdown", cmd_shutdown);
