.. _bluetooth-periph-pm-sample:

BLE Power management sample
###########################

Overview
********

Application to demonstrate the low power BLE operation.

Requirements
************

* Alif Balletto Development Kit
* Joulescope for power consumption measurement with included application.
* Phone application or any other BLE device to connect and subscribe to

Building and Running
********************

This sample can be found under :zephyr_file:`samples/bluetooth/le_periph_pm` in the
sdk-alif tree.

When flashing the application Ensure that there is no tracing happening in Secure Enclave by modifying the device configuration.

from file: add-device-config.json set the SE_BOOT_INFO equals 2

.. code-block:: console

    {
      "id": "SE_BOOT_INFO",
      "value": 2
    }

Enable the following configurations to get the most optimal  BLE power settings.

.. code-block:: console

    CONFIG_ALIF_EXT_WAKEUP_TIME=0
    CONFIG_ALIF_OSC_WAKEUP_TIME=1700
    CONFIG_ALIF_RM_WAKEUP_TIME=0
    CONFIG_ALIF_EXT_WARMBOOT_WAKEUP_TIME=1700
    CONFIG_SLEEP_ENABLED=y

Setting up the measurement device
*********************************

See the following descriptions how to connect the joulescope to measure the power consumption.

.. figure:: ./images/devkit_jp3_pins.png

Replace JP3 jumper with two jumper wires going to the current measurement ports.

.. figure:: ./images/devkit_j11_pins.png

Connect the ground wire to J11 ground pin example pin 1.

.. figure:: ./images/joulescope_power_measure.png

Connect the joulescope.

.. figure:: ./images/devkit_pins.png

Example Setup connections using 3.3v input voltage.

* Ground: J11 Pin 1

* Current measure: JP3 pin 1

* Current measure: JP3 pin 2

Measuring the power consumption
*******************************

When starting the application BLE is configured and there is a 10 second delay before the Device starts the sleep cycle.
BLE starts the advertisement and should be visible in the phone application as ALIF_HLO

* **Measurement point 1** BLE advertisements are now sent in 1.5s intervals and you can use the joulescope to measure the BLE advertisement power consumption.

* **Measurement point 2** ES1 wakes up every 30 seconds from RTC wakeup. This is the case where you dont have connection and do some idle housekeeping.

Scan and connect the Central application to the device
======================================================
You can use phone application or Any other BLE application to connect that supports peripheral to change connection interval.
After connection the application changes the connection interval to 1 second interval.
RTC wakeup for ES1 becomes also 1 second interval.

* **Measurement point 3** you can now measure BLE connection keep alive messages without payload

Subscribe to the data from the application
==========================================
After subscription the Application starts to send maximum payload data to the Central device in 1 second interval

* **Measurement point 4** Find an BLE wakeup with Transmission to Measure single large data transmission.

* **Measurement point 5** When BLE receives ACK for the successful transmissions it wakes the ES1 to inform the transmission so you can find a BLE reception with immediate ES1 wakeup and processing


