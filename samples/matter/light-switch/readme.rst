.. _matter-light-switch:

Matter Light Switch Sample
##########################

.. contents:: Table of Contents
   :depth: 2
   :local:

Overview
********

Sample for evaluating Matter Light Switch endpoint with Matter SDK.
Sample supports Matter commissioning using BLE to Thread network.

Requirements
************

.. note::
   Make sure all hardware is properly connected before proceeding with the setup.

- Alif Balletto Development Kit
- Matter Thread Border router and Matter Controller
- Pre-provisioned Matter Light Bulb device by ``Apple Home`` or ``chip-tool``.

Build Sample
************

.. tip::
   Make sure your development environment is properly set up before building.

.. code-block:: console

   west build -p always -b alif_b1_dk_rtss_he samples/matter/light-switch

Light Switch Endpoint
*********************

Sample has generated ZAP and Matter files which enable Matter Root endpoint 0 and OnOff Light Switch and Generic Switch endpoints.

Endpoint 1 OnOff Light Switch Server / Client cluster. Client toggles light by using Devkit Joystick center button (Press down).
Server cluster is optional but it is activated for supporting Apple Home ecosystem. Apple will request server for subscribed data model,
so button press will update On/Off Light Switch server state which is reported to Apple Matter Controller. User needs to define Automation for Switch event Turn On or Turn Off.

Endpoint 2 Generic Switch for switch server support that sends event notifications ``InitialPress`` and ``ShortRelease``.
Endpoint uses joystick left top corner button. Apple supports only single press events.

User Interface
**************

Devkit uses 2 LEDs for indicating Thread and BLE communication state:

Blue LED:

- Blinking slowly indicates that Matter BLE Advertisement is active for Matter Provisioning
- Fast Blink (100ms) Means that BLE connection is active and Matter Provisioning is ongoing

Red LED:

- Fast Blinking (100ms) indicates Thread network is active for joining provisioned network
- 1 Second constant Active Mode indicates that Matter controller subscription is established or resumed
- Single short Flash indicates endpoint activation

Sample supports Balletto Devkit Joystick buttons.

Center Button ``SW0``:

- Press down will toggle OnOff-switch endpoint state

Left Up Corner Button ``SW1``:

- Generic switch to generate ``InitialPress`` at active state and ``ShortRelease`` after releasing the button.

Right Up Corner Button ``SW2``:

- Hold over 3 seconds for Matter factory reset. After Factory reset press DK normal Reset Button for new device provisioning.

Provisioning with Apple Home
****************************

.. note::
   You will need an iPhone or iPad with the Apple Home app installed and connected to the same WiFi network as your Matter controller.

1. Flash the built application to the device and press the ``Reset`` button.
#. After reset, you should see a slowly blinking blue LED which indicates that the device is ready for Matter provisioning.
#. Open the ``Apple Home`` application with iPhone or iPad which is connected to the same WiFi network as the Matter Controller and Thread Border Router.
#. Click ``+`` button from app for ``Add Accessory``.
#. Scan QR code

   .. image:: QRCode.png

#. Click ``Add To Home`` to start provisioning to Thread network and Matter fabric.
#. You should see now fast blinking Blue LED when BLE connection is established.
#. ``Apple Home`` will warn about Uncertified Accessory so just click ``Add Anyway`` to continue.
#. When provisioning is ready ``Apple Home`` asks where to add light, select a room and click ``Continue``.
#. You can rename the default name to ``Matter Switch`` and click ``Continue``.
#. Click ``Continue``
#. Click at next window ``View at Home`` and you can see light switch.

Device is now provisioned and will report automatically if user presses ``SW0`` On/Off Light Switch or ``SW1`` Generic Switch.
Device is now provisioned and can control provisioned Matter Light(s) by adding ``Automation`` in ``Apple Home`` for the connected Matter device light switch.

Light-switch automation with Apple Home
***************************************

Light on and off need separate ``Automation``. The following example flow is for a Matter Accessory named ``Matter Light``:

Creating Light On ``Automation``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. tip::
   Setting up automations allows your light switch to control your light bulb.

1. Click ``+`` button from app for ``Add Automation``.
#. Select ``An Accessory is Controlled`` for use ``Matter Switch`` controlling a ``Matter Light``.
#. Select ``Matter Switch`` from list or your own named light switch device and click ``Next``.
#. Next page select ``Turn On`` and click ``Next``.
#. Select from list ``Matter Light`` and click ``Next``.
#. Click ``Matter Light`` light icon for setting ``Turn on`` select dimming level and click ``Done``.

Creating Light Off ``Automation``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. tip::
   This automation complements the Light On automation for complete control.

1. Click ``+`` button from app for ``Add Automation``.
#. Select ``An Accessory is Controlled`` for use ``Matter Switch`` controlling a ``Matter Light``.
#. Select ``Matter Switch`` from list or your own named light switch device and click ``Next``.
#. Next page select ``Turn Off`` and click ``Next``.
#. Select from list ``Matter Light`` and click ``Next``.
#. Click ``Matter Light`` light icon for setting ``Turn off`` set dimming level to 0 click ``Done``.

Now ``Matter Switch`` can control ``Matter Light`` by pressing DK ``SW0`` center button or using virtual switch from ``Apple Home`` application.

Provisioning with chip-tool
***************************

.. note::
   This section assumes you have the chip-tool installed and a Thread border router set up.

Using ``chip-tool``, you need to know Thread network active dataset and Device QR-code payload.

Manual pair code:
`34970112332`

QR Payload:
`MT:4CT91AFN00KA0648G00`

How to get Thread network Active data set:

Open a terminal on your Linux system which is running ``Open Thread Border Router`` and run the following command:

.. code-block:: console

    sudo ot-ctl dataset active -x
    35060004001fffe00c0402a0f7f8051000112233445566778899aabbccddee00030e4f70656e54687265616444656d6f0410445f2b5ca6f2a93a55ce570a70efeecb000300001a02081111111122222222010212340708fd110022000000000e0800000003601c0000
    Done

Provision device:

1. Flash the built binary to the device and press the ``Reset`` button
#. After reset, you should see a slowly blinking blue LED which indicates that the device is ready for Matter provisioning
#. Start ``chip-tool`` in interactive mode by the following command:

    .. code-block:: console

        chip-tool interactive start

#. Provision the device using ``chip-tool`` and assign ``node_id`` 2 and QR code Payload ``MT:4CT91AFN00KA0648G00`` with the following command:

.. code-block:: console

    pairing code-thread 2 hex:35060004001fffe00c0402a0f7f8051000112233445566778899aabbccddee00030e4f70656e54687265616444656d6f0410445f2b5ca6f2a93a55ce570a70efeecb000300001a02081111111122222222010212340708fd110022000000000e0800000003601c0000 MT:6FCJ142C00KA0648G00 --bypass-attestation-verifier true

#. The blue LED starts blinking faster when the device BLE connection is established and provisioning starts to Thread and Matter Fabric.
#. Wait for ``chip-tool`` to finish provisioning.

Light switch is provisioned to Matter and Thread network.

Binding Light Switch to Light Bulb with chip-tool
**************************************************

.. important::
   Binding is necessary for the light switch to control the light bulb.

Using ``chip-tool``, the device needs to bind to the provisioned Light Bulb endpoint ``node-id``.

Bind On/Off switch ``node-id`` 2 to Light bulb ``node-id`` 1 by:

.. code-block:: console

    binding write binding '[{"fabricIndex": 1, "node": 1, "endpoint": 1, "cluster": 6}, {"fabricIndex": 1, "node": 1, "endpoint": 1, "cluster": 8}]' 2 1

Now the Light Switch device can toggle the Light Bulb device by pressing the DK ``SW0`` center button.
