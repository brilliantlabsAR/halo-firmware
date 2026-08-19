.. _bluetooth-periph-rscps-sample:

BLE Running Speed And Cadence Sample
####################################

Overview
********

Application to demonstrate the use of the Running Speed and Cadence BLE profile.

Requirements
************

* Alif Balletto Development Kit

Building and Running
********************

This sample can be found under :zephyr_file:`samples/bluetooth/le_periph_rscps` in the
sdk-alif tree.

When running, the sample application starts advertising and waits for a central to connect.
After connection, the central should start notifications and the peripheral will send measurements every 2 seconds.
Notifications for battery service are handled independently.
