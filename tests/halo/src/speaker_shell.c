#include <string.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>


#ifdef CONFIG_AUDIO
#include <zephyr/drivers/audio.h>
#include "audio.h"

LOG_MODULE_REGISTER(speaker_shell, LOG_LEVEL_DBG);

#define SPEAKER_NODE DT_CHOSEN(zephyr_speaker)

#if !DT_NODE_HAS_STATUS(SPEAKER_NODE, okay)
#error "Unsupported board: speaker devicetree node is not defined"
#endif

static const struct device *dev = DEVICE_DT_GET(SPEAKER_NODE);

static k_tid_t tid;
static struct k_thread speaker_thread;
static K_THREAD_STACK_DEFINE(speaker_thread_stack, 1024);
static bool is_initialized = false;
static bool is_playing = false;
static bool is_continuous = false;
static uint8_t *_file = NULL;
static uint32_t _file_size = 0;

static void speaker_fill(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);
	int err = 0;
	const struct shell *sh = (const struct shell *)arg1;

	uint32_t remaining = _file_size;
	while (is_playing) {
		size_t bytes_written = 0;
		err = audio_write(dev, _file + _file_size - remaining, remaining,
				  &bytes_written, K_MSEC(1000));
		if (err != 0 && err != -ETIMEDOUT) {
			shell_error(sh, "Failed to write to audio device: %d\n", err);
			return;
		}
		remaining -= bytes_written;
		LOG_DBG("%d/%d %d bytes written", _file_size - remaining, _file_size, bytes_written);
		if (remaining == 0) {
			if(is_continuous) {
				remaining = _file_size;
				LOG_INF("Restarting audio playback (continuous mode)");
			}
			else{
				is_playing = false;
				LOG_INF("Audio playback completed");
			}
		}
		/* Small delay to avoid overwhelming the system */
		k_sleep(K_MSEC(10));
	}
}

static int cmd_get_device(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!dev) {
		shell_error(sh, "No speaker device found");
		return -ENODEV;
	}
	shell_print(sh, "Speaker Device: %s", dev->name);
	return err;
}

static int cmd_speaker_init(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	
	if (!device_is_ready(dev)) {
		shell_error(sh, "Speaker device not ready");
		return -ENODEV;
	}

	/* Configure audio stream using generic interface */
	struct audio_config config = {
		.sample_rate = 8000,
		.bits_per_sample = 16,
		.channels = AUDIO_MONO,
		.is_signed = true,
	};

	err = audio_configure(dev, &config);
	if (err) {
		shell_error(sh, "Failed to configure audio: %d\n", err);
		return err;
	}

	/* Set default volume */
	err = audio_set_volume(dev, 75);
	if (err) {
		shell_warn(sh, "Failed to set volume (may not be supported): %d\n", err);
		/* Continue anyway as volume control may not be supported */
	}

	is_initialized = true;
	is_playing = false;

	shell_print(sh, "Speaker initialized successfully");
	return 0;
}

static int cmd_speaker_deinit(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	
	if (!device_is_ready(dev)) {
		shell_error(sh, "Speaker device not ready");
		return -ENODEV;
	}
	
	/* Stop audio playback if currently playing */
	if (is_playing) {
		err = audio_trigger(dev, AUDIO_TRIGGER_STOP);
		if (err) {
			shell_error(sh, "Failed to stop audio: %d\n", err);
			return err;
		}
		is_playing = false;
	}
	
	is_initialized = false;
	shell_print(sh, "Speaker deinitialized successfully");
	return 0;
}

static int cmd_speaker_play(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	
	if (!device_is_ready(dev)) {
		shell_error(sh, "Speaker device not ready");
		return -ENODEV;
	}

	if (!is_initialized) {
		shell_error(sh, "Speaker not initialized\n");
		return -EINVAL;
	}

	if (is_playing) {
		shell_error(sh, "Already playing\n");
		return -EALREADY;
	}

	int file = atoi(argv[1]);
	
	if (argc == 3) {
		is_continuous = true;
		shell_print(sh, "Continuous playback mode enabled");
	} else {
		is_continuous = false;
	}

	/* Start audio playback using generic interface */
	err = audio_trigger(dev, AUDIO_TRIGGER_START);
	if (err) {
		shell_error(sh, "Failed to start audio: %d\n", err);
		return err;
	}

	/* Select audio file based on parameter */
	if (file % 2 == 0) {
		_file = (uint8_t *)_acfemale_w1_8k_16bit_EQ7;
		_file_size = sizeof(_acfemale_w1_8k_16bit_EQ7);
		shell_print(sh, "Playing female voice sample");
	} else if(file % 2 == 1) {
		_file = (uint8_t *)_acmono_8k_16bit_1k_wave;
		_file_size = sizeof(_acmono_8k_16bit_1k_wave);
		shell_print(sh, "Playing mono sine wave");
	}

	is_playing = true;

	tid = k_thread_create(&speaker_thread, speaker_thread_stack,
			      K_THREAD_STACK_SIZEOF(speaker_thread_stack), 
			      speaker_fill, (void *)sh, NULL,
			      NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);

	shell_print(sh, "Audio playback started");
	return 0;
}

static int cmd_speaker_stop(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	
	if (!device_is_ready(dev)) {
		shell_error(sh, "Speaker device not ready");
		return -ENODEV;
	}

	if (!is_initialized) {
		shell_error(sh, "Speaker not initialized\n");
		return -EINVAL;
	}

	if (!is_playing) {
		shell_error(sh, "Not playing\n");
		return -EALREADY;
	}

	/* Stop the playback thread first */
	is_playing = false;

	/* Stop audio using generic interface */
	err = audio_trigger(dev, AUDIO_TRIGGER_STOP);
	if (err) {
		shell_error(sh, "Failed to stop audio: %d\n", err);
		return err;
	}

	shell_print(sh, "Audio playback stopped");
	return 0;
}
static int cmd_speaker_volume(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	
	if (!device_is_ready(dev)) {
		shell_error(sh, "Speaker device not ready");
		return -ENODEV;
	}

	int volume = atoi(argv[1]);
	if (volume < 0 || volume > 100) {
		shell_error(sh, "Volume must be between 0 and 100");
		return -EINVAL;
	}

	/* Set volume using generic interface */
	err = audio_set_volume(dev, volume);
	if (err) {
		shell_error(sh, "Failed to set volume: %d (may not be supported)\n", err);
		return err;
	}
	
	shell_print(sh, "Volume set to %d", volume);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_speaker,
	SHELL_CMD_ARG(get_device, NULL, "get the speaker device", cmd_get_device, 1, 0),
	SHELL_CMD_ARG(init, NULL, "initialize the speaker device with generic audio interface", cmd_speaker_init, 1, 0),
	SHELL_CMD_ARG(deinit, NULL, "deinitialize the speaker device", cmd_speaker_deinit, 1, 0),
	SHELL_CMD_ARG(play, NULL, "play audio file (file_number [continuous])", cmd_speaker_play, 2, 1),
	SHELL_CMD_ARG(stop, NULL, "stop playing audio", cmd_speaker_stop, 1, 0),
	SHELL_CMD_ARG(volume, NULL, "set the volume of the speaker device (0-100)", cmd_speaker_volume, 2,
		      0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(speaker, &sub_speaker, "Generic Audio Interface Speaker Commands", NULL);

#endif