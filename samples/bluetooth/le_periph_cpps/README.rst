.. _bluetooth-periph-cpps-sample:

BLE Cycling Power Sample
########################

Overview
********

Application to demonstrate the use of the Cycling Power BLE profile.

Requirements
************

* Alif Balletto Development Kit

Building and Running
********************

This sample can be found under :zephyr_file:`samples/bluetooth/le_periph_cpps` in the
sdk-alif tree.

When running, the sample application starts advertising and waits for a central to connect.
After connection, the central should start notifications and the peripheral will send Instantaneous Power measurements every 2 seconds.
