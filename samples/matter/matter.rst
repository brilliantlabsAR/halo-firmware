.. _alif-matter-samples:

Alif Matter samples
###################

* light-bulb: Matter light can be controlled by Matter controller or Matter light switch.
* light-switch: Matter light switch which can be bind to Matter light by Matter controller for control.

.. code-block::

    samples/matter
    ├── light-bulb
    │   ├── b1_factorydata_partition.overlay
    │   ├── CMakeLists.txt
    │   ├── factory_data.conf
    │   ├── Kconfig
    │   ├── prj.conf
    │   ├── readme.rst
    │   └── src
    │       ├── AppTask.cpp
    │       ├── include
    │       │   ├── AppConfig.h
    │       │   ├── AppEvent.h
    │       │   ├── AppTask.h
    │       │   ├── BoardUtil.h
    │       │   └── CHIPProjectConfig.h
    │       ├── lighting-app.matter
    │       ├── lighting-app.zap
    │       ├── main.cpp
    │       └── ZclCallbacks.cpp
    └── light-switch
        ├── b1_factorydata_partition.overlay
        ├── CMakeLists.txt
        ├── factory_data.conf
        ├── Kconfig
        ├── prj.conf
        ├── readme.rst
        └── src
            ├── AppTask.cpp
            ├── include
            │   ├── AppConfig.h
            │   ├── AppEvent.h
            │   ├── AppTask.h
            │   ├── BoardUtil.h
            │   ├── CHIPProjectConfig.h
            │   ├── LightSwitch.h
            │   └── ShellCommands.h
            ├── LightSwitch.cpp
            ├── light-switch-app.matter
            ├── light-switch-app.zap
            ├── main.cpp
            ├── ZclCallbacks.cpp
            └── ShellCommands.cpp


Prerequites for Matter SDK envrimonent
**************************************

Matter open source project provides a script that creates an own Python virtual environment and installs ZAP tools.
See the [Introduce to Matter SDK project](https://project-chip.github.io/connectedhomeip-doc/getting_started/first_example.html) documentation.

The SDK have a own script calls Matter bootstrap script installs all necessary Python packets, builds Matter SDK Host tools and adds the tools to PATH with following commands:

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


Light switch sample
*******************

This sample demonstrates a device acting as a light switch which controls a light bulb
Build sample with following commands:

.. code-block:: console

    cd samples/matter/light-switch \
    west build -b alif_b1_dk_rtss_he


Light bulb sample
*****************

This sample demonstrates a device acting as a light bulb and how to bind it to a light switch or used direct by Matter controller.
Build sample with following commands:

.. code-block:: console

    cd samples/matter/light-bulb \
    west build -b alif_b1_dk_rtss_he

Matter light control demo with Apple Home
*****************************************

This Chapter defines a flow for how to evaluate Matter Light and Switch to ``Apple Home`` automation system.

Requirements
============

1. 2 Balletto DK for Matter devices.
#. Apple TV or Apple HomePod
#. Ipad or Iphone with ``Apple Home``

Provision light-bulb device
===========================

1. Build and Flash ``light-bulb`` sample and Reset a Device
#. Provision ``light-bulb`` by scanning device Scanning sample QR code by ``Apple Home`` and new Accessory to ``Matter Light``.
   Check More details from ``/light-bulb/readme.rst``.
#. ``Matter Light`` is provisioned and you shuold see device at ``Apple Home`` Home window.
#. Test led toggle from ``Apple Home`` by clicking ``Matter Light`` light bulb icon.

Provision light-switch device
=============================

1. Build and Flash ``light-switch`` sample and Reset a Device
#. Provision ``light-switch`` by scanning device Scanning sample QR code by ``Apple Home`` and new Accessory to ``Matter Switch``.
   Check More details from ``/light-switch/readme.rst``.
#. ``Matter Switch`` is provisioned and you shuold see device at ``Apple Home`` Home window 2 new Matter accessory with different type.
#. Add ``Automation`` to ``Matter Switch`` ``SW0``-button for control ``Matter Light`` green led.
   Check More details from ``/light-switch/readme.rst`` how to add ``Automation``.
#. Test Led Toggle by clicking DK Joystick Center button ``SW0``

Matter light control demo with chip-tool
****************************************

Requirements
============

1. 2 Balletto DK for Matter devices.
#. nRF8240 USB Dongle for Thread Border router with Network RPC Co-Processor binary
#. Ubuntu 22.04 PC for acting as a Border router

Prerequites for OpenThread Border Router
========================================

Please check ``/subsys/matter/otbr_setup.rst`` how to build and configure OpenThread Border Router device.

Getting started
===============

With ``chip-tool`` user defines ``node-id`` and in this demo will be used following numbers:
* light-bulb, 1
* light-switch, 2

How to get Thread network Active data set
=========================================

Open terminal to your linux which is running ``Open Thread Border router`` and call next command:

.. code-block:: console

    sudo ot-ctl dataset active -x
    35060004001fffe00c0402a0f7f8051000112233445566778899aabbccddee00030e4f70656e54687265616444656d6f0410445f2b5ca6f2a93a55ce570a70efeecb000300001a02081111111122222222010212340708fd110022000000000e0800000003601c0000
    Done

Start chip-tool to interactive mode
===================================

Start `chip-tool` in interactive mode by the following command:

.. code-block:: console

    chip-tool interactive start

Interactive mode is enabled to call multiple commands without timeouts.

Commission Matter devices
=========================

Flash the light bulb device and commission that to Thread network and use endpoint ``node_id`` 1 and QR code ``MT:6FCJ142C00KA0648G00``:

.. code-block:: console

    pairing code-thread 1 hex:35060004001fffe00c0402a0f7f8051000112233445566778899aabbccddee00030e4f70656e54687265616444656d6f0410445f2b5ca6f2a93a55ce570a70efeecb000300001a02081111111122222222010212340708fd110022000000000e0800000003601c0000 MT:6FCJ142C00KA0648G00 --bypass-attestation-verifier true

Flash the light switch device and commission that to Thread network and use endpoint ``node_id`` 2 and QR code ``MT:4CT9142C00KA0648G00``:

.. code-block:: console

    pairing code-thread 2 hex:35060004001fffe00c0402a0f7f8051000112233445566778899aabbccddee00030e4f70656e54687265616444656d6f0410445f2b5ca6f2a93a55ce570a70efeecb000300001a02081111111122222222010212340708fd110022000000000e0800000003601c0000 MT:6FCJ142C00KA0648G00 --bypass-attestation-verifier true

Bind the light switch to the light bulb
=======================================

Update Light bulb's access list to include the light switch by the following command:

.. code-block:: console

    accesscontrol write acl '[{"fabricIndex": 1, "privilege": 5, "authMode": 2, "subjects": [112233], "targets": null}, {"fabricIndex": 1, "privilege": 3, "authMode": 2, "subjects": [2], "targets": [{"cluster": 6, "endpoint": 1, "deviceType": null}, {"cluster": 8, "endpoint": 1, "deviceType": null}]}]' 1 0

Next, bind the light switch device to the light bulb device:

.. code-block:: console

    binding write binding '[{"fabricIndex": 1, "node": 1, "endpoint": 1, "cluster": 6}, {"fabricIndex": 1, "node": 1, "endpoint": 1, "cluster": 8}]' 2 1

Now the light switch device can control the light bulb device's led.

Light control
=============

The light switch sample supports three commands.

* Switch on: ``matter light on``
* Switch off:``matter light off``
* Toggle light state:``matter light toggle``

The ``chip-tool`` controller can be used too control a commissioned Matter light device.

.. code-block::

    onoff <command> <destination-id> <endpoint-id>

    `command` Light control command:
    * `on`: Switch On
    * `off`: Switch Off
    * `toggle`: Toggle Light state
    `destinatio-id` is device commisioned `node_id`.
    `endpoint-id` is 1.


Example for toggling ``node_id`` 1's light state:

.. code-block:: console

    onoff toggle 1 1


.. toctree::
   :maxdepth: 1
   :glob:

   **/*