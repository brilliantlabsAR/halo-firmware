.. _shipmode_sample:

Ship Mode Sample
################

Overview
********

This sample demonstrates using the ship mode driver to shutdown the device into
ultra-low power state. After a countdown, the device shuts down and requires
hardware reset to wake up.

Building and Running
********************

.. code-block:: console

   west build -b halo samples/halo/sm
   west flash

Sample Output
*************

.. code-block:: console

   === Ship Mode Sample ===

   WARNING: Device will shutdown in 5 seconds
   Hardware reset required to wake up!

   Shutdown in 5...
   Shutdown in 4...
   Shutdown in 3...
   Shutdown in 2...
   Shutdown in 1...

   Shutting down now!
   [00:00:05.000,000] <wrn> shipmode: Shutting down - entering ship mode

Device Tree Configuration
*************************

.. code-block:: devicetree

   sm: sm {
       compatible = "sm-gpio";
       gpios = <&gpio6 0 GPIO_ACTIVE_HIGH>;
       status = "okay";
   };

   aliases {
       shutdown = &sm;
   };
