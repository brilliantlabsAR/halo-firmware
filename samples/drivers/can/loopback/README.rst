.. _CAN-sample:

CAN Sample
####################

Overview
********

This sample demonstrates using the Cast CAN driver.
This sample performs loopback communication.

Supported Targets
*****************
* alif_e7_dk_rtss_hp
* alif_e7_dk_rtss_he
* alif_e3_dk_rtss_hp
* alif_e3_dk_rtss_he
* alif_e1c_dk_rtss_he
* alif_b1_dk_rtss_he

Building and Running
********************

In loopback mode, the board receives its own messages. This could be used for
standalone testing.

The sample can be built and executed for boards with a SoC that have an
integrated CAN controller or for boards with a SoC that has been augmented
with a stand alone CAN controller.

.. code-block:: console

    *** Booting Zephyr OS build ZAS-v1.0.0-rc1-41-g52f6a2de41a6 ***
    [00:00:00.000,000] <inf> ALIF_CAN: *** CAN Loopback Demo Started ***
    [00:00:00.000,000] <inf> ALIF_CAN: Device capabilities: 15
    [00:00:00.000,000] <inf> ALIF_CAN: Set Nominal Bitrate: 500000, sample point: 800
    [00:00:00.000,000] <inf> ALIF_CAN: Set FD Bitrate: 2000000, sample point: 800
    [00:00:00.000,000] <inf> ALIF_CAN: Normal Filter ID: 1445
    [00:00:00.000,000] <inf> ALIF_CAN: Extended Filter ID: 33512026
    [00:00:00.000,000] <inf> ALIF_CAN: Sending MsgType: 0
    [00:00:00.000,000] <inf> ALIF_CAN: Msg Rx Success
    [00:00:00.010,000] <inf> ALIF_CAN: Sending MsgType: 1
    [00:00:00.010,000] <inf> ALIF_CAN: Msg Rx Success
    [00:00:00.020,000] <inf> ALIF_CAN: Sending MsgType: 2
    [00:00:00.021,000] <inf> ALIF_CAN: Msg Rx Success
    [00:00:00.031,000] <inf> ALIF_CAN: Sending MsgType: 3
    [00:00:00.031,000] <inf> ALIF_CAN: Msg Rx Success
    [00:00:00.041,000] <inf> ALIF_CAN: Sending MsgType: 4
    [00:00:00.041,000] <inf> ALIF_CAN: Msg Rx Success
    [00:00:00.052,000] <inf> ALIF_CAN: Sending MsgType: 5
    [00:00:00.052,000] <inf> ALIF_CAN: Msg Rx Success
    [00:00:00.062,000] <inf> ALIF_CAN: *** CAN Loopback Demo is over ***
