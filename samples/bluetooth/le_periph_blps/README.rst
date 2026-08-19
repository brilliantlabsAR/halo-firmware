.. _bluetooth-periph-blps-sample:

BLE Blood Pressure Sample
#########################

Overview
********

Application to demonstrate the use of the Blood Pressure BLE profile.

Requirements
************

* Alif Balletto Development Kit

Building and Running
********************

This sample can be found under :zephyr_file:`samples/bluetooth/le_periph_blps` in the
sdk-alif tree.

See :ref:`Alif bluetooth samples section <alif-bluetooth-samples>` for details.

When running, the sample application starts advertising and waits for a central to connect.
After connection, the central should start notifications and the peripheral will send measurements every second.
Notifications for battery service are handled independently.
