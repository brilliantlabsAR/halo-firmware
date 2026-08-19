.. _bluetooth-periph-cgms-sample:

BLE Continuous Glucose Monitor Sample
#####################################

Overview
********

Application to demonstrate the use of the Continuous Glucose Monitor BLE profile.

Requirements
************

* Alif Balletto Development Kit

Building and Running
********************

This sample can be found under :zephyr_file:`samples/bluetooth/le_periph_cgms` in the
sdk-alif tree.

When running, the sample application starts advertising and waits for a central to connect.
After connection, the central should start notifications and the peripheral will send measurements every second.
Notifications for battery service are handled independently.
