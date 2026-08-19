#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <max98357a_audio.h>
#include "audio.h"

LOG_MODULE_REGISTER(max98357a_audio_sample, LOG_LEVEL_INF);

static const struct device *speaker = DEVICE_DT_GET(DT_CHOSEN(zephyr_speaker));

int main(void)
{
	int ret = 0;
	size_t bytes_written;

	printk("MAX98357A Audio Driver Sample\n");

	if (!device_is_ready(speaker)) {
		LOG_ERR("Speaker device not ready");
		return -1;
	}

	/* Configure audio stream */
	struct max98357a_audio_stream_cfg config = {
		.sample_rate = 16000,
		.bits = 16,
		.mode = MAX98357A_AUDIO_MONO,
		.is_signed = true,
	};

	ret = max98357a_audio_configure(speaker, &config);
	if (ret) {
		LOG_ERR("Failed to configure MAX98357A audio: %d", ret);
		return ret;
	}

	/* Set volume */
	ret = max98357a_audio_set_volume(speaker, 75);
	if (ret) {
		LOG_ERR("Failed to set volume: %d", ret);
		return ret;
	}

	/* Start audio playback */
	ret = max98357a_audio_trigger(speaker, MAX98357A_AUDIO_TRIGGER_START);
	if (ret) {
		LOG_ERR("Failed to start MAX98357A audio: %d", ret);
		return ret;
	}

	LOG_INF("Starting audio playback...");

	/* Play audio buffer continuously with dynamic volume demo */
	uint32_t loop_count = 0;
	uint32_t remaining_bytes = sizeof(female_w1_16k_16bit_EQ7_raw);
	/* Cycle through these volume levels */
	static const uint8_t demo_volumes[] = {100, 70, 40, 10, 0, 10, 40, 70};
	int vol_index = 0;
	uint32_t last_vol_change = k_uptime_get_32();
	const size_t chunk_size = 1024; /* Write in 1KB chunks */
	while (1) {
		const size_t to_write = MIN(chunk_size, remaining_bytes);
		ret = max98357a_audio_write(speaker, female_w1_16k_16bit_EQ7_raw + 
					    sizeof(female_w1_16k_16bit_EQ7_raw) - remaining_bytes, 	to_write, &bytes_written,
					    K_MSEC(1000));
		if (ret != 0 && ret != -ETIMEDOUT) {
			LOG_ERR("Failed to write to MAX98357A audio: %d", ret);
			break;
		}

		remaining_bytes -= bytes_written;

		if(remaining_bytes == 0) {
			LOG_INF("Audio playback completed");
			remaining_bytes = sizeof(female_w1_16k_16bit_EQ7_raw);
		}

		loop_count++;

		/* Periodically change volume every ~2 seconds */
		uint32_t now = k_uptime_get_32();
		if ((now - last_vol_change) > 2000U) {
			vol_index = (vol_index + 1) % ARRAY_SIZE(demo_volumes);
			uint8_t new_vol = demo_volumes[vol_index];
			if (max98357a_audio_set_volume(speaker, new_vol) == 0) {
				LOG_INF("[VOLUME] -> %u%%", new_vol);
			}
			last_vol_change = now;
		}

		/* Print status every 100 loops with current volume */
		if (loop_count % 100 == 0) {
			LOG_INF("Playing... (loop %d) vol=%u%%", loop_count, demo_volumes[vol_index]);
		}

		/* Small delay to avoid overwhelming the system */
		k_sleep(K_MSEC(10));
	}

	/* Stop audio playback */
	ret = max98357a_audio_trigger(speaker, MAX98357A_AUDIO_TRIGGER_STOP);
	if (ret) {
		LOG_ERR("Failed to stop MAX98357A audio: %d", ret);
	}

	return 0;
}