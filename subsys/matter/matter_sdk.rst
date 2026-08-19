.. _matter_sdk:

Matter SDK
##########

This chapter introduces Matter SDK and how to take it into use.
See the `Introduction to Matter SDK project <https://project-chip.github.io/connectedhomeip-doc/index.html>`_ documentation about Matter SDK Open source project and `Open Thread Border Router <https://openthread.io/guides/border-router>`_ documentation.

Matter controller
*****************

There are available commercial devices which support Matter controller, for example, an Apple HomePod or Apple TV for Apple Home, and a Google Nest Hub for Google Home.
Apple and Google Home require a mobile device for provisioning and Matter device configuration.

Matter SDK includes open source Matter Controller ``chip-tool`` which will be built when the environment is activated. ``chip-tool`` provides the possibility to evaluate Matter SDK for provisioning devices to Thread network and Matter fabric infrastructure.
Commercial tools are more user-friendly but ``chip-tool`` is a good tool for development purposes.

Thread Border Router
********************

Apple HomePod or Apple TV for Apple Home, and Google Nest Hub provide native support for Thread Border Router which supports Matter service requirements.

Open Thread Border Router gives the possibility to use a Raspberry Pi or Linux PC. Open Thread Border Router is a good solution with the ``chip-tool`` Matter controller.
Please check ``/subsys/matter/otbr_setup.rst`` how to build and configure OpenThread Border Router device.

.. _matter-getting-start:

Getting started
***************

Alif Matter SDK is using Zephyr platform setup with OpenThread. So initial setup for Zephyr environment is needed.
This chapter defines how to set up the Matter environment for Linux Ubuntu 22.04 based operating system and how to configure Thread Border Router for Matter.

Prerequisites for Ubuntu 22.04
==============================

Install dependencies:

.. code-block:: console

    sudo apt-get install git gcc g++ pkg-config libssl-dev libdbus-1-dev \
        libglib2.0-dev libavahi-client-dev ninja-build python3-venv python3-dev \
        python3-pip unzip libgirepository1.0-dev libcairo2-dev libreadline-dev


Matter SDK helper scripts
=========================

Alif SDK has helper scripts to create a Matter build environment and building it.
These scripts need to be called from the Alif SDK root folder.

.. code-block::

    scripts/matter
    ├── activate_env.sh
    └── matter_env_setup.sh


Matter subsystem
================

Matter subsystem is a glue layer between a Matter open source project and a Zephyr application.
It defines an API for initializing Matter stack and its main features.

.. code-block::

    subsys/matter
    ├── binding
    │   ├── BindingHandler.cpp
    │   └── BindingHandler.h
    ├── common
    │   ├── FabricTableDelegate.cpp
    │   ├── FabricTableDelegate.h
    │   ├── MatterStack.cpp
    │   ├── MatterStack.h
    │   ├── MatterUi.cpp
    │   └── MatterUi.h
    ├── icd
    │   ├── icdHandler.cpp
    │   └── icdHandler.h
    └── pwmdevice
        ├── PWMDevice.cpp
        └── PWMDevice.h

Create Python virtual env and install Matter tools
==================================================

Matter open source project provides a script that creates an own Python virtual environment and installs ZAP tools.
The SDK has its own script that calls the Matter bootstrap script, installs all necessary Python packages, builds Matter SDK Host tools, and adds the tools to PATH with the following command:

.. code-block:: console

    source scripts/matter/matter_env_setup.sh


After running this installation script, Matter SDK build system is ready for compiling the samples.

Before building Matter ZAP or Chip-Tool, Matter virtual environment must be activated with following command:

.. code-block:: console

    source scripts/matter/activate_env.sh


If the activate script says the environment is out of date, you can update it by running the following command:

.. code-block:: console

    source scripts/matter/matter_env_setup.sh


Deactivate Matter build environment with the following command:

.. code-block:: console

    deactivate


Commission device to Thread network over BLE
********************************************

After flashing, the sample device will activate a default private Thread network named ``ot_zephyr``. The device will also activate BLE advertisement for commissioning over BLE.
Thread and Matter commissioning is handled by Matter SDK's ``chip-tool`` which is built and added to ``PATH``, or by ``Apple Home`` using iPhone or iPad.

Commissioning requirements for Apple Home
=========================================

For ``Apple Home`` you need to have a device QR code which is scanned by mobile device camera.

1. Flash the built application to the device and press the ``Reset`` button.
#. After reset, you should see a slowly blinking blue LED which indicates that the device is ready for Matter provisioning.
#. Open the ``Apple Home`` application with iPhone or iPad which is connected to the same WiFi network as the Matter Controller and Thread Border Router.
#. Click the ``+`` button in the app for ``Add Accessory``.
#. Scan device QR code

#. Click ``Add To Home`` to start provisioning to Thread network and Matter fabric.
#. You should now see fast blinking blue LED when BLE connection is established.
#. ``Apple Home`` will warn about an Uncertified Accessory, so just click ``Add Anyway`` to continue.
#. When provisioning is ready ``Apple Home`` asks where to add light, select a room and click ``Continue``.
#. Set accessory name and click ``Continue``.
#. Click ``Continue``
#. Click at next window ``View at Home``.

Commissioning requirements for chip-tool
========================================

``chip-tool`` commissioning over BLE to Thread network command API:

.. code-block:: console

    $ chip-tool pairing code-thread <node_id> hex:<operational_dataset> <payloa_or_paircoded> --bypass-attestation-verifier true


Before starting the process, we need to know the following parameters:

* ``node_id``: Device Node identifier given by the user
* ``operational_dataset``: Thread Border Router active dataset
* ``payload_or_paircode``: Device QR code payload or manual pair code

How to Get Thread Border Router Active Dataset
==============================================

Border Router active dataset is checked by the following command:

.. code-block:: console

    sudo ot-ctl dataset active -x
    35060004001fffe00c0402a0f7f8051000112233445566778899aabbccddee00030e4f70656e54687265616444656d6f0410445f2b5ca6f2a93a55ce570a70efeecb000300001a02081111111122222222010212340708fd110022000000000e0800000003601c0000
    Done


How to get Device information:
==============================

Matter shell has a command ``matter onboardingcodes ble`` to get onboarding configuration.

Example from Light-bulb sample:

.. code-block::

    uart:~$ matter onboardingcodes ble
    QRCode:            MT:6FCJ142C00KA0648G00
    QRCodeUrl:         https://project-chip.github.io/connectedhomeip/qrcode.html?data=MT%3A6FCJ142C00KA0648G00
    ManualPairingCode: 34970112332
    Done

