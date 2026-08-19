BMA580 Trigger Sample
======================

This sample demonstrates the BMA580 accelerometer sensor with tap detection
and data reading capabilities. It configures the sensor for single and double
tap detection on the Z-axis, sets the sampling frequency to 200 Hz, and
continuously reads and displays accelerometer data and temperature.

Overview
--------

The sample performs the following operations:

1. Initializes the BMA580 sensor via I2C interface
2. Sets the sampling frequency to 200 Hz
3. Configures single and double tap detection on Z-axis
4. Registers interrupt handlers for tap events
5. Continuously reads and displays sensor data every 2 seconds
6. Manually checks interrupt status registers

Building and Running
--------------------

Build the sample for the Alif Halo board:

.. code-block:: console

   west build -b halo alif/samples/halo/bma580_trigger

Flash and run the sample:

.. code-block:: console

   west flash

Expected Output
---------------

The sample will print sensor readings and tap detection messages:

.. code-block::

   Tap trigger set successfully
   Double tap trigger set successfully
   bma580 sensor samples: 0

   accel x:0.001 ms/2 y:0.002 ms/2 z:9.812 ms/2 temp:25.0 C
   .
   bma580 sensor samples: 1

   accel x:0.001 ms/2 y:0.002 ms/2 z:9.810 ms/2 temp:25.0 C
   .
   Single tap triggered at 1234
   Manual check: STAP interrupt status set

When you tap the board, you should see tap detection messages and interrupt
status updates.

Hardware Requirements
---------------------

- Alif Halo board with BMA580 sensor connected via I2C0
- Interrupt-capable GPIO pins configured for BMA580 interrupts
