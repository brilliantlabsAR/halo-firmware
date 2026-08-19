.. _qmc6308_sample:

QMC6308 3-Axis Magnetometer Sample
###################################

Overview
********

This sample demonstrates the QST QMC6308 3-axis magnetometer sensor, which
provides high-precision magnetic field measurements for compass heading,
position detection, and magnetic anomaly detection applications.

The QMC6308 is a low-power, high-precision magnetic sensor with:

* 3-axis magnetic field measurement (X, Y, Z)
* Multiple selectable measurement ranges (±2G to ±30G)
* Configurable output data rates (10Hz to 1500Hz)
* Programmable oversampling and downsampling
* I2C digital interface
* Ultra-low power consumption

The sample continuously reads magnetic field data on all three axes and
displays the measurements every 2 seconds.

Features
********

**Magnetic Field Measurement**:
  - 3-axis simultaneous measurement (X, Y, Z)
  - 16-bit resolution per axis
  - Multiple measurement ranges for different applications
  - Real-time data acquisition

**Configurable Parameters**:
  - **Output Data Rate (ODR)**: 10Hz, 50Hz, 100Hz, 200Hz, 1500Hz
  - **Measurement Range**: ±2G, ±8G, ±12G, ±30G (1 Gauss = 100 µT)
  - **Oversampling Ratio**: 1x, 2x, 4x, 8x for noise reduction
  - **Downsampling Ratio**: 1x, 2x, 4x, 8x for power optimization

**Operating Modes**:
  - **Continuous Mode**: Automatic periodic measurements at configured ODR
  - **Single Mode**: One-shot measurement on demand
  - **Normal Mode**: Ready for measurement
  - **Suspend Mode**: Low-power standby

**Power Management**:
  - Automatic power control via regulator
  - Suspend mode for ultra-low power
  - Power-efficient measurement cycles

Building and Running
********************

Build and flash the sample:

.. code-block:: console

   west build -b halo samples/halo/qmc6308
   west flash

Sample Output
*************

.. code-block:: console

   [00:00:00.100,000] <inf> QMC6308: QMC6308 initialized successfully
   QMC6308 sensor samples: 0

   magn x:0.126667 ms/2 y:-0.040000 ms/2 z:0.266667 ms/2
   QMC6308 sensor samples: 1

   magn x:0.120000 ms/2 y:-0.046667 ms/2 z:0.260000 ms/2
   QMC6308 sensor samples: 2

   magn x:0.126667 ms/2 y:-0.040000 ms/2 z:0.266667 ms/2
   ...

Output fields:
  - **magn x/y/z**: Magnetic field strength in Gauss (G) on each axis
  - **ms/2**: Unit notation (should be read as Gauss)
  - Values represent the magnetic field vector in 3D space

Device Tree Configuration
*************************

The QMC6308 sensor is configured in the device tree at I2C address 0x2C:

.. code-block:: devicetree

   &i2c0 {
       status = "okay";
       
       qmc6308: qmc6308@2c {
           compatible = "qst,qmc6308";
           reg = <0x2c>;
           vin-supply = <&sen_1v8>;
           status = "okay";
           
           /* Optional configuration */
           range = <30>;          /* ±30 Gauss */
           odr = <200>;           /* 200 Hz */
           mode = "Continuous";   /* Continuous measurement */
           ovs = <8>;             /* 8x oversampling */
           ds = <8>;              /* 8x downsampling */
       };
   };

   / {
       aliases {
           zephyr,magn = &qmc6308;
       };
   };

Configuration Parameters
************************

**range** (Measurement Range):
  - **30**: ±30 Gauss (default) - LSB: 1000 nT/LSB
  - **12**: ±12 Gauss - LSB: 2500 nT/LSB
  - **8**: ±8 Gauss - LSB: 3750 nT/LSB
  - **2**: ±2 Gauss - LSB: 15000 nT/LSB (highest resolution)

  Choose lower ranges for higher resolution in low magnetic field environments.

**odr** (Output Data Rate):
  - **10**: 10 Hz - Lowest power consumption
  - **50**: 50 Hz
  - **100**: 100 Hz
  - **200**: 200 Hz (default) - Balanced performance
  - **1500**: 1500 Hz - Highest rate (requires Continuous mode)

  Note: 1500Hz is only supported in Continuous mode.

**mode** (Operating Mode):
  - **"Suspend"**: Low-power standby, no measurements
  - **"Normal"**: Ready state, measurements on request (default)
  - **"Single"**: One-shot measurement per sample_fetch call
  - **"Continuous"**: Automatic periodic measurements at ODR rate

**ovs** (Oversampling Ratio):
  - **8**: 8x oversampling (default) - Best noise reduction
  - **4**: 4x oversampling
  - **2**: 2x oversampling
  - **1**: No oversampling - Fastest measurement

  Higher oversampling reduces noise but increases power consumption.

**ds** (Downsampling Ratio):
  - **8**: 8x downsampling (default)
  - **4**: 4x downsampling
  - **2**: 2x downsampling
  - **1**: No downsampling

  Downsampling reduces effective ODR and power consumption.

**vin-supply** (Power Supply):
  Reference to regulator providing sensor power (typically 1.8V I/O supply).

API Usage
*********

Basic magnetometer reading:

.. code-block:: c

   #include <zephyr/drivers/sensor.h>

   const struct device *magn = DEVICE_DT_GET_ONE(qst_qmc6308);

   if (device_is_ready(magn)) {
       struct sensor_value x, y, z;

       /* Fetch sensor data */
       sensor_sample_fetch(magn);

       /* Get individual axis values */
       sensor_channel_get(magn, SENSOR_CHAN_MAGN_X, &x);
       sensor_channel_get(magn, SENSOR_CHAN_MAGN_Y, &y);
       sensor_channel_get(magn, SENSOR_CHAN_MAGN_Z, &z);

       printk("Magnetic field - X: %d.%06d G, Y: %d.%06d G, Z: %d.%06d G\\n",
              x.val1, x.val2, y.val1, y.val2, z.val1, z.val2);
   }

Read all axes at once:

.. code-block:: c

   struct sensor_value magn[3];

   sensor_sample_fetch(magn_dev);
   sensor_channel_get(magn_dev, SENSOR_CHAN_MAGN_XYZ, magn);

   printk("X: %f G, Y: %f G, Z: %f G\\n",
          sensor_value_to_double(&magn[0]),
          sensor_value_to_double(&magn[1]),
          sensor_value_to_double(&magn[2]));

Runtime Configuration:

.. code-block:: c

   /* Change sampling frequency to 100 Hz */
   struct sensor_value odr = { .val1 = 100, .val2 = 0 };
   sensor_attr_set(magn_dev, SENSOR_CHAN_MAGN_XYZ,
                   SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);

   /* Change range to ±8 Gauss */
   struct sensor_value range = { .val1 = 8, .val2 = 0 };
   sensor_attr_set(magn_dev, SENSOR_CHAN_MAGN_XYZ,
                   SENSOR_ATTR_FULL_SCALE, &range);

Sensor Channels
===============

The QMC6308 driver supports the following sensor channels:

* **SENSOR_CHAN_MAGN_X**: X-axis magnetic field
* **SENSOR_CHAN_MAGN_Y**: Y-axis magnetic field
* **SENSOR_CHAN_MAGN_Z**: Z-axis magnetic field
* **SENSOR_CHAN_MAGN_XYZ**: All three axes (returns array of 3 values)

Calibration and Usage
*********************

Hard Iron Calibration
=====================

Magnetometers require calibration to compensate for hard iron effects (constant
magnetic field offsets from nearby ferromagnetic materials):

1. Rotate the sensor through all orientations in 3D space
2. Record minimum and maximum values for each axis
3. Calculate offsets: ``offset = (max + min) / 2``
4. Apply: ``calibrated = raw - offset``

Example calibration values for a typical environment:

======= ========= ========= =======
Axis    Min (G)   Max (G)   Offset
======= ========= ========= =======
X       -0.450    +0.350    -0.050
Y       -0.420    +0.380    -0.020
Z       -0.520    +0.280    -0.120
======= ========= ========= =======

Compass Heading Calculation
============================

Calculate magnetic heading (azimuth) from X and Y components:

.. code-block:: c

   #include <math.h>

   double heading = atan2(magn_y, magn_x) * (180.0 / M_PI);
   if (heading < 0) {
       heading += 360.0;
   }
   printk("Heading: %.1f degrees\\n", heading);

Note: This assumes the sensor is level. For tilted sensors, compensate using
accelerometer data.

Earth's Magnetic Field
======================

Typical Earth magnetic field values:

* **Total field**: ~25-65 µT (0.25-0.65 Gauss) depending on location
* **Horizontal component**: ~15-30 µT at mid-latitudes
* **Vertical component**: ~20-60 µT (varies with latitude)

Use the ±2G range for maximum resolution when measuring Earth's field only.

Applications
************

**Electronic Compass**:
  Calculate heading/azimuth for navigation applications.

**Position Sensing**:
  Detect proximity to magnets for contactless position detection.

**Magnetic Anomaly Detection**:
  Identify ferromagnetic objects or current-carrying wires.

**Orientation Detection**:
  Combined with accelerometer/gyroscope for 9-axis IMU systems.

**Current Sensing**:
  Non-contact measurement of current in wires (Hall effect).

Troubleshooting
***************

**Readings show all zeros or don't change**:
  - Verify I2C connection and address (0x2C)
  - Check power supply (vin-supply regulator enabled)
  - Ensure sensor is in correct mode (not Suspend)
  - Verify chip ID (should be 0x80)

**Noisy or erratic readings**:
  - Increase oversampling ratio (ovs)
  - Move away from sources of magnetic interference
  - Use lower measurement range for better resolution
  - Reduce ODR if not needed

**Readings saturated at maximum**:
  - Strong magnetic field exceeds current range
  - Increase range setting (e.g., from 2G to 8G or 30G)
  - Move sensor away from strong magnets or motors

**Power consumption too high**:
  - Use lower ODR setting
  - Increase downsampling ratio
  - Use Single mode instead of Continuous
  - Enable Suspend mode when not measuring

**Data ready timeout errors**:
  - Verify ODR configuration matches expected rate
  - Check if sensor is in correct operating mode
  - Ensure sufficient time between measurements in Single mode

Performance Considerations
**************************

**Power vs. Accuracy Trade-offs**:

========== ===== ===== ===== ============ ==============
Mode       ODR   OVS   DS    Current      Use Case
========== ===== ===== ===== ============ ==============
Low Power  10Hz  1x    8x    ~10 µA       Battery apps
Balanced   100Hz 4x    2x    ~50 µA       General use
High Speed 200Hz 8x    1x    ~100 µA      Fast motion
Ultra Fast 1500Hz 1x   1x    ~200 µA      High dynamics
========== ===== ===== ===== ============ ==============

**Measurement Time**:
  - Single measurement: ~1ms + (ODR period)
  - Continuous mode: Automatic at ODR interval
  - Data ready timeout: 100ms maximum

Configuration Options
*********************

Kconfig options in ``prj.conf``:

.. code-block:: kconfig

   # Core drivers
   CONFIG_I2C=y
   CONFIG_SENSOR=y

   # Power management
   CONFIG_PM_DEVICE=y

   # Floating point support (required for sensor_value_to_double)
   CONFIG_CBPRINTF_FP_SUPPORT=y

   # Logging
   CONFIG_LOG=y
   CONFIG_SENSOR_LOG_LEVEL_INF=y

Hardware Setup
**************

**I2C Connection**:
  - **SCL**: I2C clock line
  - **SDA**: I2C data line
  - **Address**: 0x2C (fixed)
  - **Pull-ups**: Required on SCL and SDA (typically 4.7kΩ)

**Power Supply**:
  - **VDD**: 1.62V - 3.6V power supply
  - **VDDIO**: 1.62V - 3.6V I/O voltage
  - Typically connected to 1.8V sensor supply rail

**Interrupt Pin** (optional):
  - **DRDY**: Data ready interrupt output
  - Can be used for interrupt-driven data acquisition

References
**********

* `QMC6308 Product Page <https://www.qstcorp.com/en_comp_prod/QMC6308>`_
* QMC6308 Datasheet
* Zephyr Sensor API Documentation
* I2C Driver Documentation
