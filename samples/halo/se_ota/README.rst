.. zephyr:code-sample:: se_ota
   name: SE OTA Update

A sample application for updating the Security Element (SE) firmware via Over-The-Air (OTA).

Overview
********

This sample demonstrates how to perform an OTA update of the Security Element (SE) firmware on Alif Semiconductor devices.
It supports two update modes: PUPD (Partial Update) and FUPD (Full Update).
The application checks the current SE firmware revision and updates it to a target version if necessary.
The update process includes version comparison, address alignment handling, and automatic reboot after successful update.

Firmware Requirements
*********************

The SE firmware images must be provided and embedded into the application binary during the build process.
Two firmware files are supported:

- ``pupd.h``: Contains the PUPD firmware data (referenced as ``_acpupd`` symbol)
- ``fupd.h``: Contains the FUPD firmware data (referenced as ``_acfupd`` symbol)

Users need to obtain the appropriate SE firmware files and ensure they are linked into the application.
The update mode is selected via the ``UPDATE_MODE`` macro in ``main.c`` (set to ``PUPD`` or ``FUPD``).

Building and Running
********************

This application can be built and executed on supported Alif boards with SE support.

.. code-block:: console

    west build -b halo alif/samples/halo/se_ota

To select the update mode, modify the ``UPDATE_MODE`` macro in ``src/main.c``:

- Set ``UPDATE_MODE`` to ``PUPD`` for partial updates
- Set ``UPDATE_MODE`` to ``FUPD`` for full updates

Sample Output
=============

For FUPD mode:

.. code-block:: console

    Revision is SES A5 SE_FW_1.108.000-RC5 v1.108.0 Sep 29 2025 18:46:42
    Updating FUPD image in SE...2000C590 -> 5880c590 of size 169216
    SE Service: Updating STOC image at address: 0x5880C590 of size: 169216 bytes
    SE FUPD update successful, wait rebooting...
    ********************
    *** Booting Zephyr OS build zas-v1.2-206-g75011edc3ef6 ***
    Revision is SES A5 SE_FW_1.108.000-RC6 v1.108.0 Oct  8 2025 19:41:38
    SE firmware is already up to date.
    .

For PUPD mode:

.. code-block:: console

    Revision is SES A5 SE_FW_1.108.000-RC5 v1.108.0 Sep 29 2025 18:46:42
    Updating PUPD image in SE...2000C590 -> 5880c590 of size 91920
    SE Service: Updating STOC image at address: 0x5880C590 of size: 91920 bytes
    SE PUPD update successful, wait rebooting...
    ********************
    *** Booting Zephyr OS build zas-v1.2-206-g75011edc3ef6 ***
    Revision is SES A5 SE_FW_1.108.000-RC6 v1.108.0 Oct  8 2025 19:41:38
    SE firmware is already up to date.
    .
