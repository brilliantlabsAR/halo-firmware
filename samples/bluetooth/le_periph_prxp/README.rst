.. _bluetooth-periph-prxp-sample:

BLE Proximity Profile Sample
############################

Overview
********

Application to demonstrate the use of the Proximity profile BLE profile.

## Overview

An application that sends an alert on link loss with the alert level set by the central.
Battery service is supported.

Requirements
************

* Alif Balletto Development Kit

Building and Running
********************

This sample can be found under :zephyr_file:`samples/bluetooth/le_periph_prxp` in the
sdk-alif tree.

When running, the sample application starts advertising and waits for a central to connect.
After connection, the central should set the level of Link Loss alert.
If the link is lost, the peripheral will notify the application with the selected alert level.
Notifications for battery service are handled independently.
