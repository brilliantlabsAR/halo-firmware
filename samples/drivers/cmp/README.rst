.. _CMP-sample:

CMP Sample
==========

Overview
********

This sample demonstrates the usage of the Alif CMP driver.

Building and Running
********************

The application will build only for a target that has a devicetree entry with
:dt:`alif,cmp` as a compatible.

.. note::

   - This sample application demonstrates the **cmp0** functionality. To use **lpcmp**, enable the respective devicetree property.
   - On boards such as **alif_e7_dk_rtss_hp**, the **cmp2** and **cmp3** pins are connected to **uart2** pins. Before building, update.
     the configuration to use **uart4** instead of **uart2**.
   - **LPCMP functionality is supported** on `alif_e7_dk_rtss_hp` and `alif_e7_dk_rtss_he` but no output pinouts available on board.

Supported Tagets for cmp and lpcmp
**********************************
* alif_e3_dk_rtss_hp
* alif_e3_dk_rtss_he
* alif_e7_dk_rtss_hp
* alif_e7_dk_rtss_he
* alif_e1c_dk_rtss_he
* alif_b1_dk_rtss_he

CMP Console Output
******************
.. code-block:: console

    *** Booting Zephyr OS build Zephyr-Alif-SDK-v0.5.0-17-g17b360353343 ***
    [00:00:02.000,000] <inf> ALIF_CMP: start comparing
    [00:00:02.050,000] <inf> ALIF_CMP: positive input voltage is greater than negative input voltage
    [00:00:02.101,000] <inf> ALIF_CMP: negative input voltage is greater than the positive input voltage
    [00:00:02.151,000] <inf> ALIF_CMP: positive input voltage is greater than negative input voltage
    [00:00:02.201,000] <inf> ALIF_CMP: negative input voltage is greater than the positive input voltage
    [00:00:02.251,000] <inf> ALIF_CMP: positive input voltage is greater than negative input voltage
    [00:00:02.301,000] <inf> ALIF_CMP: negative input voltage is greater than the positive input voltage
    [00:00:02.351,000] <inf> ALIF_CMP: positive input voltage is greater than negative input voltage
    [00:00:02.401,000] <inf> ALIF_CMP: negative input voltage is greater than the positive input voltage
    [00:00:02.451,000] <inf> ALIF_CMP: positive input voltage is greater than negative input voltage
    [00:00:02.501,000] <inf> ALIF_CMP: negative input voltage is greater than the positive input voltage
    [00:00:02.501,000] <inf> ALIF_CMP: Comparison Completed

LPCMP Console Output
********************
.. code-block:: console

    *** Booting Zephyr OS build Zephyr-Alif-SDK-v0.5.0-17-g17b360353343 ***
    [00:00:02.000,000] <inf> ALIF_CMP: start comparing
    [00:00:02.501,000] <inf> ALIF_CMP: Comparison Completed
