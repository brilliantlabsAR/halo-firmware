.. _bluetooth-periph-glps-sample:

BLE Glucose Profile Sample
##########################

Overview
********

Application to demonstrate the use of the Glucose Profile BLE profile.

Requirements
************

* Alif Balletto Development Kit

Building and Running
********************

This sample can be found under :zephyr_file:`samples/bluetooth/le_periph_glps` in the
sdk-alif tree.

When running, the sample application starts advertising and waits for a central to connect.
After connection, the central should start notifications and the peripheral will send measurements every 2 seconds.
Notifications for battery service are handled independently.
