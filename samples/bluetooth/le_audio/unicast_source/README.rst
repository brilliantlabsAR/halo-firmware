.. _bluetooth-unicast-source-sample:

BLE Unicast Audio Source Sample
#################################

Overview
********

This sample demonstrates the LE unicast audio initiator (client) use-case.

The initiator device name can be configured using ``CONFIG_BLE_DEVICE_NAME``.

Acceptor device name can be configured using ``CONFIG_BLE_PERIPHERAL_NAME`` configuration option.
This is used to choose the correct unicast audio server for a connection. This is handled as a
prefix when searching multiple acceptor devices.

Sample automatically discovers available unicast audio server's capabilities (PAC and ASCS) and
chooses the one that best matches the codec configuration. Stream count is limited to two
(stereo LEFT and RIGHT channels). Mono mode can be used as well if the peripheral reports only
a single channel (ASE). App defaults to unidirectional mode and both left and right audio channels
are handled by a single initiator.

Sample app supports bidirectional mode by setting ``CONFIG_LE_AUDIO_BIDIRECTIONAL=y`` and
sink and source streams will be set up. Left and right audio channels are handled by
separate initiators (3 development kits are needed for this). Two DKs are enough when left or
right channel is used in bidirectional mode.
Overlay file ``bidir.conf`` in the :zephyr_file:`samples/bluetooth/le_audio/unicast_source` can
be used to configure bidirectional mode.

Codec configuration is handled automatically based on the acceptor's capabilities.
Sample will search for the best matching codec configuration and use that.


Building and Running
********************

This sample can be found under :zephyr_file:`samples/bluetooth/le_audio/unicast_source` in the sdk-alif tree.

See :ref:`Alif bluetooth samples section <alif-bluetooth-samples>` for details.
