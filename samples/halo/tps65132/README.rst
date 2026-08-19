.. _tps65132_sample:

TPS65132 Dual Output LCD Bias Regulator Sample
###############################################

Overview
********

This sample demonstrates the TPS65132 dual output LCD bias power supply IC from
Texas Instruments. The TPS65132 provides VPOS (positive) and VENG (negative)
voltage rails commonly used for LCD display panels.  

The sample toggles both regulators on and off every 10 seconds, demonstrating
voltage rail control using the Zephyr regulator API.

Hardware
********

The TPS65132 provides:

* VPOS output: Positive voltage rail (4.0V to 5.5V in 100mV steps)
* VENG output: Negative voltage rail (-4.0V to -4.5V in 100mV steps)
* I2C interface for voltage configuration
* GPIO enable pins for each output
* Input voltage supply (VIN)

.. warning::
   
   **Hardware Limitation**: Due to hardware design constraints on this board,
   the voltage ranges are limited:
   
   - VPOS: Maximum 5.5V (not 6.0V as per chip spec)
   - VENG: Maximum -4.5V (not -6.0V as per chip spec)
   
   These limits are enforced in the driver to prevent hardware damage.
   DO NOT modify the voltage range settings without confirming hardware compatibility!

Building and Running
********************

Build and flash:

.. code-block:: console

   west build -b halo samples/halo/tps65132
   west flash

Sample Output
*************

.. code-block:: console

   Regulator Control Demo
   VPOS enabled
   VENG enabled
   VPOS disabled
   VENG disabled
   VPOS enabled
   VENG enabled
   ...

The output shows the regulators being enabled and disabled in a 10-second cycle.

Device Tree Configuration
*************************

The TPS65132 is configured in the device tree:

.. code-block:: devicetree

   &i2c1 {
       tps65132: tps65132@3e {
           compatible = "ti,tps65132";
           reg = <0x3e>;
           vin-supply = <&cam_1v8>;
           supply-gpios = <&gpio2 1 GPIO_ACTIVE_HIGH>;
           apps = "40ma";
           
           veng: VENG {
               regulator-name = "veng";
               regulator-init-microvolt = <4500000>;
               enable-gpios = <&gpio5 6 GPIO_ACTIVE_HIGH>;
               startup-delay-us = <5000>;
               status = "okay";
           };

           vpos: VPOS {
               regulator-name = "vpos";
               regulator-init-microvolt = <5500000>;
               enable-gpios = <&gpio7 5 GPIO_ACTIVE_HIGH>;
               startup-delay-us = <5000>;
               status = "okay";
           };
           status = "okay";
       };
   };

Configuration
*************

Key parameters:

* **regulator-init-microvolt**: Initial voltage in microvolts

  - VPOS: 5.5V (5500000 µV) - **Maximum safe value for this hardware**
  - VENG: 4.5V (4500000 µV, magnitude only) - **Maximum safe value for this hardware**

* **enable-gpios**: GPIO pins to enable each regulator
* **startup-delay-us**: Delay after enabling (5ms)
* **apps**: Output current capability (40mA)

Voltage Ranges
==============

The driver enforces these voltage ranges for hardware protection:

**VPOS (Positive Rail)**:
  - Range: 4.0V to 5.5V
  - Step: 100mV
  - Values: 4.0V, 4.1V, 4.2V, ... 5.4V, 5.5V (16 steps)
  - Register: 0x00

**VENG (Negative Rail)**:
  - Range: -4.0V to -4.5V  
  - Step: 100mV
  - Values: -4.0V, -4.1V, -4.2V, -4.3V, -4.4V, -4.5V (6 steps)
  - Register: 0x01

.. code-block:: c

   /* Driver enforced ranges (from regulator_tps65132.c) */
   static const struct linear_range vpos_buck_range = 
       LINEAR_RANGE_INIT(4000000, 100000U, 0x0U, 0x0FU);  // 4.0V-5.5V
   
   static const struct linear_range veng_buck_range = 
       LINEAR_RANGE_INIT(4000000, 100000U, 0x0U, 0x05U);  // 4.0V-4.5V

API Usage
*********

The sample uses the standard Zephyr regulator API:

.. code-block:: c

   #include <zephyr/drivers/regulator.h>

   /* Get regulator devices */
   const struct device *vpos = DEVICE_DT_GET(DT_NODELABEL(vpos));
   const struct device *veng = DEVICE_DT_GET(DT_NODELABEL(veng));

   /* Check if ready */
   if (device_is_ready(vpos) && device_is_ready(veng)) {
       /* Enable regulators */
       regulator_enable(vpos);
       regulator_enable(veng);

       /* Disable regulators */
       regulator_disable(vpos);
       regulator_disable(veng);
   }

Applications
************

The TPS65132 is commonly used for:

* LCD and OLED display bias voltage
* Display panel power supply
* Positive and negative rail generation
* Embedded displays requiring dual polarity supplies

References
**********

* `TPS65132 Datasheet <https://www.ti.com/product/TPS65132>`_
* Zephyr Regulator API Documentation
