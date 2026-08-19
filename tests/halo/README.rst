.. _halo_test:

Halo Board Comprehensive Test Suite
####################################

Overview
********

This test suite provides comprehensive verification of all hardware components
and drivers on the Halo board. It demonstrates proper initialization and usage
of various subsystems including button input, audio sensor (T5838), and more.

The test application features:

* GPIO button event detection (single/double click, multi-level long press)
* T5838 audio activity detection (AAD) with low-power wake-up
* Shell interface for interactive testing of peripherals
* Comprehensive peripheral coverage (display, sensors, audio, camera, etc.)

This is useful for:

* Board bring-up and hardware verification
* Driver functionality testing
* System integration validation
* Reference implementation for production applications

Components Tested
*****************

**Input Devices**:
  - GPIO button (sw0 alias): All event types supported
  - Button debouncing and event timing verification

**Audio Sensors**:
  - T5838 AAD Mode A: Ambient audio detection
  - Configurable threshold (90dB default) and LPF (2.0kHz)
  - Low-power wake-up capability

**Shell Commands** (see README.md for full reference):
  - **Speaker**: PWM audio playback control
  - **Microphone**: PDM/DMIC recording
  - **Display**: MIPI-DSI display and Canvas graphics
  - **LED**: PWM brightness control
  - **Video**: Camera capture with JPEG encoding
  - **Sensors**: Magnetometer, accelerometer, battery voltage
  - **I2C/GPIO/ADC**: Low-level hardware access

Building and Running
********************

Build and flash the test suite:

.. code-block:: console

   west build -b halo alif/tests/halo
   west flash

After flashing, connect to the UART console:

.. code-block:: console

   minicom -D /dev/ttyUSB0 -b 115200

Test Output
***********

Initial Boot
============

.. code-block:: console

   *** Booting Zephyr OS build zas-v1.2-xxx ***
   
   Button initialized: button
   T5838 AAD Mode A configured:
     - LPF: 2.0 kHz
     - Threshold: 90 dB
     - Silent Period: 3000 ms
   
   System ready. Test button or use shell commands...
   
   uart:~$

Button Events
=============

When pressing the button, you'll see:

.. code-block:: console

   Single Click: 1234
   Double Click: 5678
   Long Press Level 1: 12345
   Long Press Level 2: 18900
   Long Press Level 3: 25600

T5838 Audio Detection
=====================

When ambient audio exceeds threshold:

.. code-block:: console

   T5838 interrupt triggered
   T5838 interrupt triggered

Shell Commands
==============

The shell provides interactive control of all peripherals:

.. code-block:: console

   uart:~$ speaker init
   Speaker initialized
   
   uart:~$ speaker play 0
   Playing voice sample...
   
   uart:~$ sensor get vbat
   Voltage: 3.78 V, SoC: 64%
   
   uart:~$ display init
   Display initialized: 240x320
   
   uart:~$ video take 1 1
   Capturing frame...
   Got frame 1! size: 6400; timestamp 1234 ms
   JPEG FILE:
   /9j/4AAQSkZJRgABAQE...
   JPEG END

Device Configuration
********************

Button Configuration
====================

The button is configured via device tree:

.. code-block:: devicetree

   button: button {
       compatible = "gpio-button";
       gpios = <&lpgpio 1 GPIO_ACTIVE_LOW>;
       debounce-ms = <30>;
       long-press-ms = <1000 3000 8000 10000>;
       double-click-ms = <400>;
   };

   aliases {
       sw0 = &button;
   };

T5838 Configuration
===================

The T5838 audio sensor is on I2C:

.. code-block:: devicetree

   &i2c1 {
       t5838: t5838@38 {
           compatible = "t5838";
           reg = <0x38>;
           int-gpios = <&gpio5 4 GPIO_ACTIVE_LOW>;
       };
   };

Configuration Parameters
************************

Button Settings
===============

**Debounce Time**: 30ms (filters mechanical bounce)

**Long Press Levels**:
  - Level 0: 1000ms (1 second)
  - Level 1: 3000ms (3 seconds)
  - Level 2: 8000ms (8 seconds)  
  - Level 3: 10000ms (10 seconds)

**Double Click Window**: 400ms

T5838 AAD Mode A Settings
=========================

**Low-Pass Filter**: 2.0 kHz
  - Filters high-frequency noise
  - Optimized for speech/ambient audio

**Threshold**: 90 dB SPL
  - Trigger level for audio detection
  - Adjust based on environment

**Silent Period**: 3000ms
  - Minimum time between triggers
  - Prevents repeated interrupts

Shell Command Reference
***********************

Speaker Commands
================

.. code-block:: console

   speaker get_device          # Show device name
   speaker init                # Initialize (8kHz, 16-bit mono)
   speaker deinit              # Stop and deinitialize
   speaker play <id> [loop]    # Play sample (0: voice, 1: tone)
   speaker stop                # Stop playback
   speaker volume <0-100>      # Set volume

Microphone Commands
===================

.. code-block:: console

   micphone get_device         # Show device name
   micphone init               # Initialize (8kHz, 16-bit, stereo)
   micphone deinit             # Stop and deinitialize
   micphone record             # Start recording (prints samples)
   micphone stop               # Stop recording

Display Commands
================

.. code-block:: console

   display get_device          # Show device name
   display init                # Initialize DSI/panel/canvas
   display deinit              # Turn off display
   display fill <RGB_hex>      # Fill screen (e.g., FF0000 for red)
   display brightness <0-255>  # Set backlight
   display draw                # Cycle through test patterns

LED Commands
============

.. code-block:: console

   led get_device              # Show device name
   led init                    # Initialize LED driver
   led brightness <0-255>      # Set brightness

Video Commands
==============

.. code-block:: console

   video get_device            # Show device name
   video init                  # Initialize camera
   video deinit                # Stop and release
   video take <n> <show>       # Capture n frames
                               # n: 1=single, -1=continuous, 0=stop
                               # show: 0=info only, 1=JPEG output

Sensor Commands
===============

.. code-block:: console

   sensor get qmc6308@2c       # Magnetometer (X, Y, Z in µT)
   sensor get bma580@18        # Accelerometer (X, Y, Z in m/s²)
   sensor get vbat             # Battery voltage and SoC

System Commands
===============

.. code-block:: console

   # GPIO control
   gpio conf <device> <pin> <flags>
   gpio get <device> <pin>
   gpio set <device> <pin> <value>
   
   # I2C bus scan
   i2c scan <device>
   i2c read <device> <addr> <reg> <bytes>
   i2c write <device> <addr> <reg> <value>
   
   # ADC reading
   adc read <device> <channel>
   
   # Regulator control
   regulator enable <device>
   regulator disable <device>
   regulator vset <device> <voltage_uV>
   
   # File system
   ls [path]
   cat <file>
   mkdir <dir>
   rm <file>

Usage Examples
**************

Audio Playback Test
===================

.. code-block:: console

   uart:~$ speaker init
   uart:~$ speaker volume 80
   uart:~$ speaker play 0
   # Listen for audio output
   uart:~$ speaker stop
   uart:~$ speaker deinit

Audio Recording Test
====================

.. code-block:: console

   uart:~$ micphone init
   uart:~$ micphone record
   # Watch PDM data stream
   # r: 0123 4567 89AB CDEF ...
   uart:~$ micphone stop
   uart:~$ micphone deinit

Display Test
============

.. code-block:: console

   uart:~$ display init
   uart:~$ display fill FF0000      # Red
   uart:~$ display brightness 128   # 50%
   uart:~$ display draw             # Test patterns
   uart:~$ display deinit

Camera Capture
==============

.. code-block:: console

   uart:~$ video init
   uart:~$ video take 1 1           # Capture 1 photo as JPEG
   # Wait for base64 output
   uart:~$ video deinit

Sensor Reading
==============

.. code-block:: console

   uart:~$ sensor get vbat
   Voltage: 3.78 V, SoC: 64%
   
   uart:~$ sensor get qmc6308@2c
   X: 12.3 µT, Y: -4.5 µT, Z: 48.2 µT
   
   uart:~$ sensor get bma580@18
   X: 0.1 m/s², Y: 0.0 m/s², Z: 9.8 m/s²

Button Testing
==============

Physical button interaction:

1. **Single Click**: Quick press and release
2. **Double Click**: Two quick presses within 400ms
3. **Long Press**: Hold for durations listed above

Watch console for event timestamps.

T5838 Wake Testing
==================

1. Ensure silent environment
2. Play audio or make noise above 90dB
3. Watch for interrupt messages
4. Verify 3-second silence period between triggers

Troubleshooting
***************

**Button not responding**:
  - Check GPIO connection (LPGPIO 1)
  - Verify device tree alias ``sw0`` exists
  - Check debounce timing (may need adjustment)

**T5838 no interrupts**:
  - Verify I2C address (0x38)
  - Check interrupt GPIO (GPIO5_4)
  - Adjust threshold if environment too quiet/noisy
  - Confirm AAD mode configuration

**Shell commands not working**:
  - Ensure ``CONFIG_SHELL=y`` is set
  - Check device initialization in main
  - Verify device tree nodes exist
  - Try ``<module> get_device`` first

**Display/Video failures**:
  - Check MIPI-DSI configuration
  - Verify voltage regulators enabled
  - Ensure sufficient heap size (64KB+)
  - Check framebuffer in DTCM

**Audio issues**:
  - Verify PWM/I2S peripherals enabled
  - Check speaker connections
  - Test with volume at 100%
  - Confirm sample rate (8kHz default)

Configuration Options
*********************

Core Configuration
==================

.. code-block:: kconfig

   CONFIG_LOG=y
   CONFIG_PRINTK=y
   CONFIG_ASSERT=y
   CONFIG_STDOUT_CONSOLE=y
   CONFIG_SHELL=y

Peripheral Drivers
==================

.. code-block:: kconfig

   CONFIG_GPIO=y
   CONFIG_GPIO_SHELL=y
   CONFIG_I2C=y
   CONFIG_I2C_SHELL=y
   CONFIG_ADC=y
   CONFIG_ADC_SHELL=y
   CONFIG_SENSOR=y
   CONFIG_SENSOR_SHELL=y
   CONFIG_REGULATOR=y
   CONFIG_REGULATOR_SHELL=y

Audio Configuration
===================

.. code-block:: kconfig

   CONFIG_MAX98357A_AUDIO=y
   CONFIG_MAX98357A_AUDIO_GENERIC_API=y
   CONFIG_I2S=y
   CONFIG_PWM=y

Display Configuration
=====================

.. code-block:: kconfig

   CONFIG_DISPLAY=y
   CONFIG_MIPI_DSI=y
   CONFIG_FB_USES_DTCM_REGION=y
   CONFIG_CANVAS=y

File System
===========

.. code-block:: kconfig

   CONFIG_FILE_SYSTEM=y
   CONFIG_FILE_SYSTEM_LITTLEFS=y
   CONFIG_FILE_SYSTEM_SHELL=y
   CONFIG_SETTINGS_FILE_PATH="/lfs/settings"

LED Configuration
=================

.. code-block:: kconfig

   CONFIG_LED_PWM=y
   CONFIG_LED_INIT_PRIORITY=50

Performance Considerations
**************************

Memory Usage
============

- **Heap**: 64KB+ recommended for video/display
- **Stack**: Default sizes sufficient for most operations
- **DTCM**: Used for display framebuffer (230KB)

Processing Time
===============

- Button events: <1ms detection latency
- T5838 interrupt: <5ms response time  
- Display updates: Immediate (framebuffer-based)
- Video capture: ~33ms per frame @ 30fps
- Audio playback: Real-time 8kHz streaming

Power Consumption
=================

- Active (all peripherals): ~100-150mA
- Display on: +50-80mA
- Camera active: +40-60mA
- T5838 AAD mode: ~200µA
- Button idle: <1µA

Extensions
**********

This test suite can be extended to include:

- Automated test sequences
- Performance benchmarking
- Stress testing (long-duration)
- Production test automation
- Calibration procedures
- OTA update verification

Related Samples
***************

* :ref:`button_sample`: Dedicated button event testing
* :ref:`vga020_sample`: Display graphics demonstration
* :ref:`battery_sample`: Battery monitoring
* :ref:`qmc6308_sample`: Magnetometer usage
* :ref:`bt_throughput_sample`: Bluetooth testing

References
**********

* Zephyr Shell Documentation
* GPIO Button Driver API
* T5838 Audio Activity Detector
* Canvas 2D Graphics Library
* Video Software Pipeline
* Sensor Subsystem Documentation
