/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

#include <zephyr/shell/shell.h>
#include <stdio.h>
#include "config.h"
#include "common.h"
#include "peripheral.h"
#include "central.h"
#include "gap.h"

LOG_MODULE_REGISTER(shell_peripheral, LOG_LEVEL_ERR);

static struct k_thread tp_thread;
static K_THREAD_STACK_DEFINE(tp_stack, CONFIG_ALIF_TP_SHELL_THREAD_STACKSIZE);

extern uint32_t supervision_to;

static int cmd_info(const struct shell *shell, size_t argc, char **argv)
{
	char temp[64];
	const enum gap_role my_role = get_device_role();

	shell_fprintf(shell, SHELL_VT100_COLOR_YELLOW, "Throughput device config:\n");
	shell_fprintf(shell, SHELL_VT100_COLOR_GREEN, "  Name: ");
	shell_fprintf(shell, SHELL_VT100_COLOR_DEFAULT, CONFIG_BLE_TP_DEVICE_NAME "\n");

	if (my_role == GAP_ROLE_NONE) {
		shell_fprintf(shell, SHELL_VT100_COLOR_GREEN, "Device type not set\n");
		return 0;
	}

	shell_fprintf(shell, SHELL_VT100_COLOR_GREEN, "  Role: ");
	shell_fprintf(shell, SHELL_VT100_COLOR_DEFAULT,
		      (my_role == GAP_ROLE_LE_PERIPHERAL) ? "Peripheral\n" : "Central\n");

	get_private_address(temp, sizeof(temp));
	shell_fprintf(shell, SHELL_VT100_COLOR_GREEN, "  Private Address: ");
	shell_fprintf(shell, SHELL_VT100_COLOR_DEFAULT, "%s\n", temp);

	peripheral_get_service_uuid_str(temp, sizeof(temp));
	shell_fprintf(shell, SHELL_VT100_COLOR_GREEN, "  Service UUID: ");
	shell_fprintf(shell, SHELL_VT100_COLOR_DEFAULT, "%s\n", temp);

	if (my_role == GAP_ROLE_LE_CENTRAL) {
		struct central_conn_params env_info;

		central_connection_params_get(&env_info);

		shell_fprintf(shell, SHELL_VT100_COLOR_GREEN, "  Connection interval min: ");
		shell_fprintf(shell, SHELL_VT100_COLOR_DEFAULT, "%d\n", env_info.conn_interval_min);

		shell_fprintf(shell, SHELL_VT100_COLOR_GREEN, "  Connection interval max: ");
		shell_fprintf(shell, SHELL_VT100_COLOR_DEFAULT, "%d\n", env_info.conn_interval_max);

		shell_fprintf(shell, SHELL_VT100_COLOR_GREEN, "  Supervision timeout: ");
		shell_fprintf(shell, SHELL_VT100_COLOR_DEFAULT, "%d\n", env_info.supervision_to);
	}

	return 0;
}

static int cmd_peripheral_start(const struct shell *sh, size_t argc, char **argv)
{
	const enum gap_role my_role = get_device_role();

	if (my_role != GAP_ROLE_NONE) {
		LOG_ERR("Device role already set to %s",
			(my_role == GAP_ROLE_LE_PERIPHERAL) ? "Peripheral" : "Central");
		return -EINVAL;
	}

	set_device_role(GAP_ROLE_LE_PERIPHERAL);

	printk("Start advertising\n");

	k_thread_create(&tp_thread, tp_stack, K_THREAD_STACK_SIZEOF(tp_stack), tp_worker, NULL,
			NULL, NULL, CONFIG_ALIF_TP_SHELL_THREAD_PRIORITY, 0, K_NO_WAIT);
	return 0;
}

static int cmd_central_start(const struct shell *sh, size_t argc, char **argv)
{
	const enum gap_role my_role = get_device_role();

	if (my_role != GAP_ROLE_NONE) {
		LOG_ERR("Device role already set to %s",
			(my_role == GAP_ROLE_LE_PERIPHERAL) ? "Peripheral" : "Central");
		return -EINVAL;
	}

	set_device_role(GAP_ROLE_LE_CENTRAL);

	k_thread_create(&tp_thread, tp_stack, K_THREAD_STACK_SIZEOF(tp_stack), tp_worker, NULL,
			NULL, NULL, CONFIG_ALIF_TP_SHELL_THREAD_PRIORITY, 0, K_NO_WAIT);

	return 0;
}

static int cmd_tp_test_start(const struct shell *sh, size_t argc, char **argv)
{
	if (get_device_role() != GAP_ROLE_LE_CENTRAL) {
		LOG_ERR("Only central device could start the test");
		return -1;
	}

	if (argc > 1) {
		const uint32_t test_time = strtol(argv[1], NULL, 10);

		if (central_set_test_duration(test_time)) {
			LOG_ERR("Invalid test time");
			return -EINVAL;
		}
	}

	LOG_DBG("start TP test.");

	if (get_app_state() == APP_STATE_CENTRAL_READY) {
		app_transition_to(APP_STATE_STATS_RESET);
	} else {
		LOG_ERR("Peripheral not ready");
	}
	return 0;
}

int cmd_set_send_interval(const struct shell *shell, size_t const argc, char **argv)
{
	if (argc < 2) {
		LOG_ERR("Invalid number of arguments");
		return -EINVAL;
	}

	if (get_device_role() != GAP_ROLE_LE_CENTRAL) {
		LOG_ERR("Only central device could define send interval");
		return -EINVAL;
	}

	const uint32_t interval = strtol(argv[1], NULL, 10);

	return central_set_send_interval(interval);
}

static int32_t param_get_int(size_t const argc, char **argv, char *p_param, int const def_value)
{
	if (p_param && argc > 1) {
		for (int n = 0; n < (argc - 1); n++) {
			if (strcmp(argv[n], p_param) == 0) {
				return strtol(argv[n + 1], NULL, 0);
			}
		}
	}
	return def_value;
}

static int cmd_set_conn_interval(const struct shell *shell, size_t const argc, char **argv)
{
	if (argc < 2) {
		LOG_ERR("Invalid number of arguments");
		return -EINVAL;
	}

	const enum gap_role my_role = get_device_role();

	if (my_role == GAP_ROLE_NONE) {
		LOG_ERR("Device role not set");
		return -EINVAL;
	}

	struct central_conn_params env_info;

	central_connection_params_get(&env_info);

	env_info.conn_interval_min = param_get_int(argc, argv, "--min", env_info.conn_interval_min);
	env_info.conn_interval_max = param_get_int(argc, argv, "--max", env_info.conn_interval_max);
	env_info.supervision_to =
		param_get_int(argc, argv, "--supervision", env_info.supervision_to);

	if (my_role == GAP_ROLE_LE_PERIPHERAL) {

		const struct peripheral_conn_params params = {
			.conn_interval_min = env_info.conn_interval_min,
			.conn_interval_max = env_info.conn_interval_max,
			.supervision_to = env_info.supervision_to,
		};

		return peripheral_connection_params_set(&params);
	}

	return central_connection_params_set(&env_info);
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_tp, SHELL_CMD_ARG(info, NULL, "Throughput device info", cmd_info, 1, 10),
	SHELL_CMD_ARG(connection, NULL,
		      "Set connection params: --min <min> --max <max> --supervision <supervision>",
		      cmd_set_conn_interval, 2, 10),
	SHELL_CMD_ARG(interval, NULL, "Set send interval (ms)", cmd_set_send_interval, 2, 10),
	SHELL_CMD_ARG(peripheral, NULL, "Peripheral config", cmd_peripheral_start, 1, 10),
	SHELL_CMD_ARG(central, NULL, "Central config", cmd_central_start, 1, 10),
	SHELL_CMD_ARG(run, NULL, "Run throughput test: <duration_s>", cmd_tp_test_start, 1, 10),
	SHELL_SUBCMD_SET_END /* Array terminated. */
);

SHELL_CMD_REGISTER(tp, &sub_tp, "Throughput device config", NULL);
