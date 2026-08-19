/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */
#include "inference_process.hpp"

#include "ethosu/models/keyword_spotting_cnn_small_int8/input.h"
#include "ethosu/models/keyword_spotting_cnn_small_int8/output.h"

#if defined(CONFIG_ARM_ETHOS_U55_256)
#include "ethosu/models/keyword_spotting_cnn_small_int8/model_u55_256.h"
#else
#include "ethosu/models/keyword_spotting_cnn_small_int8/model_u55_128.h"
#endif

#include <zephyr/shell/shell.h>
#include <inttypes.h>
#include <string>
#include <stdio.h>
#include <vector>
#include <zephyr/kernel.h>

static K_THREAD_STACK_DEFINE(ethosu_stack, CONFIG_ALIF_ETHOSU_SHELL_THREAD_STACKSIZE);
static struct k_thread ethosu_thread;
static k_sem ethosu_sem;
static atomic_t ethosu_running = 0;

__attribute__((section(".bss.tflm_arena"),
	       aligned(16))) static uint8_t tensor_arena[TENSOR_ARENA_SIZE];

static void ethosu_worker(void *, void *, void *)
{
	InferenceProcess::InferenceProcess npu(tensor_arena, TENSOR_ARENA_SIZE);
	uint32_t jobcnt = 0;
	bool status;

	while (true) {
		if (k_sem_take(&ethosu_sem, K_NO_WAIT) == 0) {
			break;
		}

		InferenceProcess::InferenceJob job(
			modelName,
			InferenceProcess::DataPtr(const_cast<uint8_t *>(networkModelData),
						  sizeof(networkModelData)),
			{InferenceProcess::DataPtr(const_cast<uint8_t *>(inputData),
						   sizeof(inputData))},
			{},
			{InferenceProcess::DataPtr(const_cast<uint8_t *>(expectedOutputData),
						   sizeof(expectedOutputData))});

		status = npu.runJob(job);
		jobcnt++;

		if ((jobcnt % 100) == 0) {
			printk("%s jobcnt=%u status=%s\n", modelName, jobcnt,
			       status ? "failed" : "ok");
		}
	}
}

static int cmd_start(const struct shell *shell, size_t, char **)
{
	if (atomic_set(&ethosu_running, 1) == 1) {
		shell_fprintf(shell, SHELL_VT100_COLOR_DEFAULT, "Ethos-U55 already inferencing\n");
		return -1;
	}

	k_sem_init(&ethosu_sem, 0, 1);

	k_thread_create(&ethosu_thread, ethosu_stack, K_THREAD_STACK_SIZEOF(ethosu_stack),
			ethosu_worker, NULL, NULL, NULL, CONFIG_ALIF_ETHOSU_SHELL_THREAD_PRIORITY,
			0, K_NO_WAIT);

	shell_fprintf(shell, SHELL_VT100_COLOR_DEFAULT, "Start Ethos-U55 inferencing\n");
	return 0;
}

static int cmd_stop(const struct shell *shell, size_t, char **)
{
	if (atomic_set(&ethosu_running, 0) == 0) {
		shell_fprintf(shell, SHELL_VT100_COLOR_DEFAULT, "Ethos-U55 already stopped\n");
		return -1;
	}

	k_sem_give(&ethosu_sem);
	k_thread_join(&ethosu_thread, K_FOREVER);

	shell_fprintf(shell, SHELL_VT100_COLOR_DEFAULT, "Stop Ethos-U55 inferencing\n");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_cmds, SHELL_CMD_ARG(start, NULL, "start", cmd_start, 1, 10),
			       SHELL_CMD_ARG(stop, NULL, "stop", cmd_stop, 1, 10),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(ethosu, &sub_cmds, "Ethos-U55 commands", NULL);
