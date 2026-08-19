.. _battery_sample:

Battery Voltage Monitoring Sample
##################################

Overview
********

This sample demonstrates battery voltage monitoring and state-of-charge (SoC)
estimation using the Alif VBAT sensor driver. The driver uses an ADC to measure
battery voltage through a resistor divider network and provides:

* Real-time battery voltage measurement
* State of Charge (SoC) percentage estimation
* Charging status detection via GPIO
* Voltage-based fuel gauge functionality

The sample continuously reads and displays battery voltage, SoC percentage,
and charging status every second.

Features
********

**Voltage Measurement**:
  - ADC-based voltage sensing through resistor divider
  - Automatic power control for low-power operation
  - Configurable measurement channel

**State of Charge (SoC)**:
  - Voltage-to-SoC conversion algorithm
  - Range: 0-100% based on 3.2V to 4.1V battery range
  - Real-time SoC updates

**Charging Detection**:
  - GPIO-based charge status monitoring
  - Automatic charging state detection
  - Optional interrupt-driven updates

**Power Management**:
  - Automatic power on/off during measurements
  - Configurable power-up delay
  - Support for regulator or GPIO enable control

Building and Running
********************

Build and flash:

.. code-block:: console

   west build -b halo samples/halo/battery
   west flash

Sample Output
*************

.. code-block:: console

   [00:00:00.100,000] <inf> ALIF_VBAT: ALIF VBAT initialized successfully
   Voltage: 3850mV, SoC: 72%, Charge Status: 1
   Voltage: 3850mV, SoC: 72%, Charge Status: 1
   Voltage: 3851mV, SoC: 72%, Charge Status: 1
   Voltage: 3852mV, SoC: 72%, Charge Status: 1
   ...

Output fields:
  - **Voltage**: Actual battery voltage in millivolts
  - **SoC**: State of Charge percentage (0-100%)
  - **Charge Status**: 1 = charging, 0 = not charging

Device Tree Configuration
*************************

The VBAT sensor is configured in the device tree:

.. code-block:: devicetree

   vbat: vbat {
       compatible = "alif,alif-vbat";
       status = "okay";
       enable-gpios = <&gpio7 7 GPIO_ACTIVE_HIGH>;
       adc = <&adc0>;
       state-gpios = <&gpio0 5 GPIO_ACTIVE_HIGH>;
       channel = <4>;
       output-ohms = <1000>;
       full-ohms = <2400>;
       power-up-time-ms = <50>;
   };

Configuration Parameters
************************

**ADC Configuration**:
  - **adc**: Reference to ADC device (e.g., ``<&adc0>``)
  - **channel**: ADC channel number for voltage measurement (0-8)
  - **power-up-time-ms**: Delay after power-on before measurement (default: 50ms)

**Resistor Divider**:
  - **output-ohms**: Lower resistor value in divider network (1000Ω)
  - **full-ohms**: Total divider resistance (2400Ω)
  - **Voltage scaling**: ``actual_voltage = adc_voltage × (full_ohms / output_ohms)``
  - **Example**: With 1000Ω and 2400Ω, scaling factor is 2.4×

**GPIO Control**:
  - **enable-gpios**: GPIO pin to enable power to voltage divider
  - **state-gpios**: GPIO pin to detect charging status (active low)

**Power Supply**:
  - **vin-supply**: Optional regulator reference for power control

Voltage to SoC Conversion
**************************

The driver uses a simple linear voltage-to-SoC mapping:

.. code-block:: c

   SoC% = (voltage_mv - 3200) / 9

Voltage-SoC table:

======= ========
Voltage SoC (%)
======= ========
3.2V    0%
3.3V    11%
3.5V    33%
3.7V    56%
3.9V    78%
4.1V    100%
======= ========

.. note::
   This is a simplified linear estimation. Real battery discharge curves
   are non-linear and vary by chemistry, temperature, and load conditions.
   For production use, consider implementing a more accurate discharge
   curve lookup table.

API Usage
*********

Basic voltage reading:

.. code-block:: c

   #include <zephyr/drivers/sensor.h>

   const struct device *vbat = DEVICE_DT_GET(DT_NODELABEL(vbat));

   if (device_is_ready(vbat)) {
       struct sensor_value voltage, soc, charge_status;

       /* Fetch sensor data */
       sensor_sample_fetch(vbat);

       /* Get actual battery voltage */
       sensor_channel_get(vbat, SENSOR_CHAN_GAUGE_VOLTAGE, &voltage);
       printk("Voltage: %dmV\\n", voltage.val1);

       /* Get state of charge */
       sensor_channel_get(vbat, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE, &soc);
       printk("SoC: %d%%\\n", soc.val1);

       /* Get charging status */
       sensor_channel_get(vbat, SENSOR_CHAN_GAUGE_STDBY_CURRENT, &charge_status);
       printk("Charging: %s\\n", charge_status.val1 ? "Yes" : "No");
   }

Sensor Channels
===============

The VBAT driver supports the following sensor channels:

* **SENSOR_CHAN_VOLTAGE**: Raw ADC voltage (after divider)
* **SENSOR_CHAN_GAUGE_VOLTAGE**: Actual battery voltage (scaled)
* **SENSOR_CHAN_GAUGE_STATE_OF_CHARGE**: Battery SoC percentage
* **SENSOR_CHAN_GAUGE_STDBY_CURRENT**: Charging status (1=charging, 0=not charging)

Hardware Design
***************

Resistor Divider Network
=========================

The battery voltage is scaled down using a resistor divider to match
the ADC input range (0-1.8V):

.. code-block:: none

   VBAT ----[ R1 ]----+----[ R2 ]---- GND
                      |
                    ADC Input

Where:
  - R1 = 1400Ω (full_ohms - output_ohms)
  - R2 = 1000Ω (output_ohms)
  - Total = 2400Ω (full_ohms)
  - Scaling = 2.4×

Example: 4.2V battery → 1.75V at ADC (within 0-1.8V range)

Power Control
=============

The voltage divider can be powered through:

1. **GPIO Enable**: Direct GPIO control (``enable-gpios``)
2. **Regulator**: Controlled via regulator framework (``vin-supply``)

This allows disabling the divider when not measuring to save power.

Charge Detection
================

Charging status is detected via a GPIO pin (``state-gpios``):

* **Low**: Battery is charging
* **High**: Battery not charging / discharging

Troubleshooting
***************

**Incorrect voltage readings**:
  - Verify resistor divider values match device tree
  - Check ADC reference voltage (should be 1.8V)
  - Ensure ADC channel number is correct

**SoC always 0% or 100%**:
  - Battery voltage may be outside 3.2V-4.1V range
  - Check voltage reading first
  - Consider adjusting voltage-to-SoC formula

**Charge status not updating**:
  - Verify ``state-gpios`` pin configuration
  - Check hardware charge detection circuit
  - Confirm GPIO polarity (active low expected)

Configuration Options
*********************

Kconfig options in ``prj.conf``:

.. code-block:: kconfig

   # Core drivers
   CONFIG_ADC=y
   CONFIG_SENSOR=y
   CONFIG_GPIO=y

   # Power management (optional)
   CONFIG_REGULATOR=y
   
   # Logging
   CONFIG_LOG=y
   CONFIG_SENSOR_LOG_LEVEL_DBG=n

   # Trigger support (optional, for charging state interrupts)
   CONFIG_ALIF_VBAT_TRIGGER=y
   CONFIG_ALIF_VBAT_TRIGGER_GLOBAL_THREAD=y

References
**********

* Zephyr Sensor API Documentation
* ADC Driver Documentation  
* Battery Management Best Practices
