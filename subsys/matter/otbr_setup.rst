.. _matter_otbr_setup:

Open Thread Border Router
#########################

This chapter introduces how to create an Open Thread Border Router for Matter SDK and how to take it into use.
See the `Open Thread Border Router <https://openthread.io/guides/border-router>`_ documentation.

Requirements
************

1. nRF52840 USB Dongle for Thread Border Router with Network RPC Co-Processor binary
#. Ubuntu 22.04 PC for acting as a Border Router

Thread Border Router Setup
==========================

This guide covers the building and configuration of the Open Thread Border Router (OTBR) with an nRF52840 USB Dongle running the Open Thread Co-Processor application.

Configure and build
===================

Clone the OTBR repository and install the default setup:

.. code-block:: console

    git clone https://github.com/openthread/ot-br-posix
    cd ot-br-posix
    ./script/bootstrap


Compile and install using the Ethernet interface:

.. code-block:: console

    INFRA_IF_NAME=eth0 ./script/setup

**NOTE:** Ethernet interface may be something else than eth0 on your system, so adjust it accordingly.

Configure RCP device
--------------------

Create a custom udev rule file to identify the nRF52840 dongle based on its vendor and product ID.

Attach the flashed RCP device to the Border Router platform via USB and check the device serial number using the following command:

.. code-block:: console

    sudo dmesg


Edit ``/etc/udev/rules.d/99-acm.rules`` and add the following line with your dongle's information:

.. code-block:: console

    SUBSYSTEM=="tty", ATTRS{idVendor}=="1915", ATTRS{idProduct}=="0000", ATTRS{serial}=="YOUR_DONGLES_SERIAL_HERE", SYMLINK+="ttyOTBR"


Check that the device is found with the given name by the following command:

.. code-block:: console

    ls /dev/tty*

You should see ``/dev/ttyOTBR``.

To configure the RCP device's serial port in the otbr-agent settings:

.. code-block:: console

    sudo nano /etc/default/otbr-agent

Edit the ``OTBR_AGENT_OPTS`` line in the following way for device ``/dev/ttyOTBR`` and ``eth0``:

.. code-block:: console

    OTBR_AGENT_OPTS="-I wpan0 -B eth0 spinel+hdlc+uart:///dev/ttyOTBR trel://eth0"

Next, power cycle the dongle to activate the Border Router and create a ``wpan0`` interface.


How to Get Thread Network Active Dataset
*****************************************

Open a terminal on your Linux system which is running ``Open Thread Border Router`` and run the following command:

.. code-block:: console

    sudo ot-ctl dataset active -x
    35060004001fffe00c0402a0f7f8051000112233445566778899aabbccddee00030e4f70656e54687265616444656d6f0410445f2b5ca6f2a93a55ce570a70efeecb000300001a02081111111122222222010212340708fd110022000000000e0800000003601c0000
    Done