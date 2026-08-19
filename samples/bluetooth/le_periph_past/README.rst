.. zephyr:code-sample:: alif_le_periph_past
   :name: Periodic Advertising Sync Transfer using Alif BLE stack

   Use the Alif BLE stack to implement a Periodic Advertising Sync Transfer (PAST) as the receiver.

Overview
********

An application demonstrating how to enable the reception of Periodic Advertising Sync Transfer
(PAST) as a peripheral.

Requirements
************

* Alif Balletto Development Kit

Building and Running
********************

This sample application can be built using the following command:

.. code-block:: console

   west build -b alif_b1_fpga_rtss_he_ble alif/samples/bluetooth/le_periph_past

When running, the sample application starts advertising and waits for a central to connect.
After connection, the central can transfer periodic synchronization to the peripheral.
