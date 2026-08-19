.. _matter-light-bulb:

Matter Light Bulb Sample
########################

.. contents:: Table of Contents
   :depth: 2
   :local:

Overview
********

Sample for how to control a green dimmable light bulb endpoint with Matter SDK.
Sample supports Matter commissioning using BLE to Thread network.

Requirements
************

.. note::
   Make sure all hardware is properly connected before proceeding with the setup.

- Alif Balletto Development Kit
- Matter Thread Border router and Matter Controller
- Pre provisioned Matter Light sample or 3rd party light switch

Build Sample
************

.. tip::
   Make sure your development environment is properly set up before building.

.. code-block:: console

   west build -p always -b alif_b1_dk_rtss_he samples/matter/light-bulb

Light Bulb Endpoint
*******************

Sample has pre-generated ZAP and Matter files that define Matter Root endpoint 0 and OnOff Dimmable Light Bulb endpoint 1.

User Interface
**************

Devkit uses 2 LEDs for indicating Thread and BLE communication state.

PWM led (Green Led):
- Dimmable Light controlled by Matter controller bonded to On/Off light switch

Blue LED:

- Blinking slowly indicates that Matter BLE Advertisement is active for Matter Provisioning
- Fast Blink (100ms) Means that BLE connection is active and Matter Provisioning is ongoing

Red LED:

- Fast Blinking (100ms) indicates Thread network is active for joining provisioned network
- 1 Second constant Active Mode indicates that Matter controller subscription is established or resumed
- Single short Flash indicates endpoint activation

Sample supports Balletto Devkit Joystick buttons

Center Button `SW0`:

- Hold over 3 seconds for Matter factory reset. After Factory reset press DK normal Reset Button for new device provisioning.

Provisioning with Apple Home
****************************

.. note::
   You will need an iPhone or iPad with the Apple Home app installed and connected to the same WiFi network as your Matter controller.

1. Flash the built application to the device and press the ``Reset`` button.
#. After reset, you should see a slowly blinking blue LED which indicates that the device is ready for Matter provisioning.
#. Open the ``Apple Home`` application with iPhone or iPad which is connected to the same WiFi network as the Matter Controller and Thread Border Router.
#. Click the ``+`` button in the app for ``Add Accessory``.
#. Scan QR code:

   .. image:: QRCode.png

#. Click ``Add To Home`` to start provisioning to Thread network and Matter fabric.
#. You should see now fast blinking Blue LED when BLE connection is established.
#. ``Apple Home`` will warn about Uncertified Accessory so just click ``Add Anyway`` to continue.
#. When provisioning is ready ``Apple Home`` asks where to add light, select a room and click ``Continue``.
#. You can rename the default name to ``Matter Light`` and click ``Continue``.
#. Click ``Continue``
#. Click at next window ``View at Home`` and you can control now Green LED by moving bar.

See the `Apple documentation for how to add device to Home app <https://support.apple.com/en-us/104998>`_.

Device is now provisioned and Light control is possible by `Apple Home` and can be used with ``Automation``.

Provisioning with chip-tool
***************************

.. note::
   This section assumes you have the chip-tool installed and a Thread border router set up.

Using ``chip-tool`` you need to know Thread network active dataset and Device QR-code payload.

Manual pair code:
`34970112332`

QR Payload:
`MT:6FCJ142C00KA0648G00`

How to get Thread network Active data set:

Open a terminal on your Linux system which is running ``Open Thread Border Router`` and run the following command:

.. code-block:: console

    sudo ot-ctl dataset active -x
    35060004001fffe00c0402a0f7f8051000112233445566778899aabbccddee00030e4f70656e54687265616444656d6f0410445f2b5ca6f2a93a55ce570a70efeecb000300001a02081111111122222222010212340708fd110022000000000e0800000003601c0000
    Done

Provision device and update access list:

Here is an example flow for provisioning a device with ``node_id`` 1:

1. Flash the built binary to the device and press the ``Reset`` button
#. After reset, you should see a slowly blinking blue LED which indicates that the device is ready for provisioning
#. Start ``chip-tool`` in interactive mode by the following command:

    .. code-block:: console

        chip-tool interactive start

#. Provision the device using ``chip-tool`` and assign ``node_id`` 1, Thread active dataset ``hex:`` and QR code Payload ``MT:6FCJ142C00KA0648G00`` with the following command:

    .. code-block:: console

        pairing code-thread 1 hex:35060004001fffe00c0402a0f7f8051000112233445566778899aabbccddee00030e4f70656e54687265616444656d6f0410445f2b5ca6f2a93a55ce570a70efeecb000300001a02081111111122222222010212340708fd110022000000000e0800000003601c0000 MT:6FCJ142C00KA0648G00 --bypass-attestation-verifier true

#. The blue LED starts blinking faster when the BLE connection is established and provisioning starts to Thread and Matter Fabric.
#. Wait for ``chip-tool`` to finish provisioning.
#. Update Light bulb's access list to include the light switch by the following command for ``node-id`` 1:

   .. code-block:: console

        accesscontrol write acl '[{"fabricIndex": 1, "privilege": 5, "authMode": 2, "subjects": [112233], "targets": null}, {"fabricIndex": 1, "privilege": 3, "authMode": 2, "subjects": [2], "targets": [{"cluster": 6, "endpoint": 1, "deviceType": null}, {"cluster": 8, "endpoint": 1, "deviceType": null}]}]' 1 0


Light Bulb is ready to be bound with light switch and be controlled by ``chip-tool``.

Controlling the LED with chip-tool
==================================

.. tip::
   These commands can be used to test your light bulb functionality after provisioning.

The ``chip-tool`` controller can be used to control a commissioned Matter light device.

.. code-block::

    onoff <command> <destination-id> <endpoint-id>

    `command` Light control command:
    * `on`: Switch On
    * `off`: Switch Off
    * `toggle`: Toggle Light state
    `destination-id` is device commissioned `node_id`.
    `endpoint-id` is 1.


Example for toggling ``node_id`` 1's light state:

.. code-block:: console

    onoff toggle 1
