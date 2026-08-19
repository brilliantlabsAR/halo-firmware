#include <string.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#ifdef CONFIG_AUDIO
#include <zephyr/audio/dmic.h>
#include <t5838.h>

#define MICPHONE_NODE DT_CHOSEN(zephyr_micphone)

#if !DT_NODE_HAS_STATUS(MICPHONE_NODE, okay)
#error "Unsupported board: micphone devicetree node is not defined"
#endif

static const struct device *dev = DEVICE_DT_GET(MICPHONE_NODE);

static k_tid_t tid;
static struct k_thread micphone_thread;
static K_THREAD_STACK_DEFINE(micphone_thread_stack, 1024);
static bool is_initialized = false;
static bool is_recording = false;

#define PDM_SAMPLE_RATE  8000
#define SAMPLE_BIT_WIDTH 16
#define NUM_CHANNELS     2
#define DURATION_MS      5000
#define INTERVAL_MS      100 // 100ms interval
#define PDM_CHANNELS     (BIT(2) | BIT(3))

#define PDM_BLOCK_SIZE      (INTERVAL_MS * PDM_SAMPLE_RATE * SAMPLE_BIT_WIDTH * NUM_CHANNELS / 8 / 1000)
#define DATA_SIZE           (PDM_BLOCK_SIZE * (DURATION_MS / INTERVAL_MS))
#define MEM_SLAB_NUM_BLOCKS (DURATION_MS / INTERVAL_MS)

// static uint8_t pdm_data[DATA_SIZE];

// every 100ms allocate a block from the slab
K_MEM_SLAB_DEFINE(mem_slab, PDM_BLOCK_SIZE, MEM_SLAB_NUM_BLOCKS, 4);

static void micphone_fill(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);
	int err = 0;
	const struct shell *sh = (const struct shell *)arg1;
	uint32_t data;
	void *buffer;
	while (is_recording) {
		err = dmic_read(dev, 0, &buffer, &data, 200);
		if (!is_recording) {
			break;
		}
		if (err) {
			shell_error(sh, "Failed to read PDM: %d\n", err);
			return;
		}
		// shell_print(sh, "Microphne Read %d bytes", data);
		shell_print(sh, "r: %04X %04X %04X %04X %04X", *((uint16_t *)buffer),
			    *((uint16_t *)buffer + 1), *((uint16_t *)buffer + 2),
			    *((uint16_t *)buffer + 3), *((uint16_t *)buffer + 4));
		k_mem_slab_free(&mem_slab, buffer);
	}
}

static int cmd_get_device(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!dev) {
		shell_error(sh, "No micphone device found");
		return -ENODEV;
	}
	shell_print(sh, "Speaker Device: %s", dev->name);
	return err;
}

static int cmd_micphone_init(const struct shell *sh, size_t argc, char *argv[])
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!dev) {
		shell_error(sh, "No micphone device found");
		return -ENODEV;
	}

	struct dmic_cfg cfg;
	struct pcm_stream_cfg stream;

	stream.pcm_width = SAMPLE_BIT_WIDTH;
	stream.pcm_rate = PDM_SAMPLE_RATE;
	stream.mem_slab = &mem_slab;
	stream.block_size = PDM_BLOCK_SIZE;

	cfg.channel.req_num_streams = 1;
	cfg.channel.req_num_chan = NUM_CHANNELS;
	cfg.channel.req_chan_map_lo = PDM_CHANNELS; // PDM channels 2 and 3
	cfg.channel.req_chan_map_hi = 0;
	cfg.streams = &stream;

	int rc = dmic_configure(dev, &cfg);
	if (rc < 0) {
		shell_error(sh, "Failed to configure PDM: %d", rc);
		return rc;
	}

	dmic_set_gain(dev, 5);

	is_initialized = true;
	is_recording = false;
	return 0;
}

static int cmd_micphone_deinit(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!dev) {
		shell_error(sh, "No micphone device found");
		return -ENODEV;
	}
	err = dmic_trigger(dev, DMIC_TRIGGER_STOP);
	if (err) {
		shell_error(sh, "Failed to start PWM audio: %d\n", err);
		return err;
	}
	is_initialized = false;
	is_recording = false;
	return 0;
}

static int cmd_micphone_record(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!dev) {
		shell_error(sh, "No micphone device found");
		return -ENODEV;
	}

	if (!is_initialized) {
		shell_error(sh, "Speaker not initialized\n");
		return 0;
	}

	if (is_recording) {
		shell_error(sh, "Already playing\n");
		return 0;
	}

	err = dmic_trigger(dev, DMIC_TRIGGER_START);
	if (err) {
		shell_error(sh, "Failed to start PWM audio: %d\n", err);
		return err;
	}

	is_recording = true;

	tid = k_thread_create(&micphone_thread, micphone_thread_stack,
			      K_THREAD_STACK_SIZEOF(micphone_thread_stack), micphone_fill, (void *)sh, NULL,
			      NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);

	return 0;
}

static int cmd_micphone_stop(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!dev) {
		shell_error(sh, "No micphone device found");
		return -ENODEV;
	}

	if (!is_initialized) {
		shell_error(sh, "Speaker not initialized\n");
		return 0;
	}

	if (!is_recording) {
		shell_error(sh, "Not playing\n");
		return 0;
	}

	err = dmic_trigger(dev, DMIC_TRIGGER_STOP);
	if (err) {
		shell_error(sh, "Failed to stop PWM audio: %d\n", err);
		return err;
	}

	is_recording = false;
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_micphone,
	SHELL_CMD_ARG(get_device, NULL, "get the micphone device", cmd_get_device, 1, 0),
	SHELL_CMD_ARG(init, NULL, "initialize the micphone device", cmd_micphone_init, 1, 0),
	SHELL_CMD_ARG(deinit, NULL, "deinitialize the micphone device", cmd_micphone_deinit, 1, 0),
	SHELL_CMD_ARG(record, NULL, "start recording a sound", cmd_micphone_record, 1, 0),
	SHELL_CMD_ARG(stop, NULL, "stop recording a sound", cmd_micphone_stop, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(micphone, &sub_micphone, "Speaker commands", NULL);

#endif // CONFIG_AUDIO