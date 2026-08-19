.. _bluetooth-periph-hr-sample:

BLE Heart Rate Sample
#####################

Overview
********

Application to demonstrate the use of the Heart Rate BLE profile.

Requirements
************

* Alif Balletto Development Kit

Building and Running
********************

This sample can be found under :zephyr_file:`samples/bluetooth/le_periph_hr` in the
sdk-alif tree.

When running, the sample application starts advertising and waits for a central to connect.
After connection, the central should start notifications and the peripheral will send measurements every second.
Notifications for battery service are handled independently.
