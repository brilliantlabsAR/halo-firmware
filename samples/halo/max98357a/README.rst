MAX98357A Audio Driver Sample
============================

This sample demonstrates how to use the MAX98357A I2S audio amplifier driver
with a PWM audio-compatible API interface.

Overview
--------

The MAX98357A driver provides a PWM audio-compatible API that makes it easy to
migrate from PWM audio to I2S audio without changing application code structure.

Features:
- PWM audio-compatible API (configure, set_volume, trigger, write)
- I2S-based audio streaming with MAX98357A amplifier
- Ring buffer for smooth audio playback
- GPIO control for SD_MODE pin
- Configurable sample rate, bit depth, and mono/stereo modeso-like Interface Sample
========================================

This sample demonstrates how to use the MAX98357A I2S audio amplifier driver
with a PWM audio-compatible interface.

Overview
--------

The MAX98357A driver provides a PWM audio-like API that makes it easy to
migrate from PWM audio to I2S audio without changing application code.

Features:
- PWM audio-compatible API (configure, set_volume, trigger, write)
- I2S-based audio streaming
- Ring buffer for smooth audio playback
- GPIO control for SD_MODE pin
- Configurable sample rate, bit depth, and mono/stereo modes

Hardware Requirements
--------------------

- Board with I2S interface
- MAX98357A breakout board connected via I2S
- Optional: GPIO pin connected to MAX98357A SD_MODE pin for shutdown control

Device Tree Configuration
------------------------

Add the following to your board's device tree:

```dts
/ {
    chosen {
        zephyr,speaker = &max98357a_speaker;
    };
};

&i2s0 {
    status = "okay";
    /* I2S pin configuration */
};

max98357a_speaker: max98357a_speaker {
    compatible = "maxim,max98357a";
    i2s = <&i2s0>;
    sd-mode-gpios = <&gpio0 10 GPIO_ACTIVE_HIGH>;  /* Optional */
};
```

Building and Running
-------------------

```bash
west build -b your_board samples/frame/max98357a_audio
west flash
```

The sample will:
1. Configure the MAX98357A for 8kHz, 16-bit mono audio
2. Set volume to 75%
3. Start audio playback
4. Continuously play a 1kHz sine wave test tone

API Usage
--------

The MAX98357A driver provides the same interface as PWM audio:

```c
#include <max98357a_audio.h>

const struct device *speaker = DEVICE_DT_GET(DT_CHOSEN(zephyr_speaker));

/* Configure audio stream */
struct max98357a_audio_stream_cfg config = {
    .sample_rate = 8000,
    .bits = 16,
    .mode = MAX98357A_AUDIO_MONO,
    .is_signed = true,
};
max98357a_audio_configure(speaker, &config);

/* Set volume (0-100) */
max98357a_audio_set_volume(speaker, 75);

/* Start playback */
max98357a_audio_trigger(speaker, MAX98357A_AUDIO_TRIGGER_START);

/* Write audio data */
size_t bytes_written;
max98357a_audio_write(speaker, audio_data, size, &bytes_written, K_MSEC(1000));

/* Stop playback */
max98357a_audio_trigger(speaker, MAX98357A_AUDIO_TRIGGER_STOP);
```

Migration from PWM Audio
-----------------------

To migrate from PWM audio to MAX98357A:

1. Replace `#include <pwm_audio.h>` with `#include <max98357a_audio.h>`
2. Replace `pwm_audio_*` function calls with `max98357a_audio_*`
3. Replace `enum pwm_audio_*` with `enum max98357a_audio_*`
4. Update device tree to use MAX98357A instead of PWM audio
5. Update Kconfig to enable MAX98357A driver

The API is intentionally identical to make migration seamless.