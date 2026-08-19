.. _bluetooth-unicast-sink-sample:

BLE Unicast Audio Sink Sample
#################################

Overview
********

This sample demonstrates the LE unicast audio acceptor (server) use-case.

The peripheral device name can be configured using ``CONFIG_BLE_DEVICE_NAME``.

The following codec configuration capabilities (PACS) are supported by this sample:

.. table:: BAP defined Codec Configuration Settings
   :widths: 2 2 2 3

   +------------------+------------------+------------------+------------------+
   | Codec            | Supported        | Supported        | Supported        |
   | Configuration    | Sampling         | Frame            | Octets per       |
   | Setting          | Frequency (kHz)  | Duration         | Codec Frame      |
   +==================+==================+==================+==================+
   | 16_1             | 16               | 7.5 ms           | 40               |
   +------------------+------------------+------------------+------------------+
   | 16_2             | 16               | 10 ms            | 40               |
   +------------------+------------------+------------------+------------------+
   | 24_1             | 24               | 7.5 ms           | 60               |
   +------------------+------------------+------------------+------------------+
   | 24_2             | 24               | 10 ms            | 60               |
   +------------------+------------------+------------------+------------------+
   | 32_1             | 32               | 7.5 ms           | 60...80          |
   +------------------+------------------+------------------+------------------+
   | 32_2             | 32               | 10 ms            | 60...80          |
   +------------------+------------------+------------------+------------------+
   | 48_1             | 48               | 7.5 ms           | 75...155         |
   +------------------+------------------+------------------+------------------+
   | 48_2             | 48               | 10 ms            | 75...155         |
   +------------------+------------------+------------------+------------------+
   | 48_4             | 48               | 10 ms            | 120..155         |
   +------------------+------------------+------------------+------------------+

Sample defaults to unidirectional mode and both left and right audio channels are
handled by a single acceptor.

Sample can be configured to support bidirectional mode where left and right audio
channels are handled by separate acceptors. This can be enabled by setting
``CONFIG_UNICAST_BIDIR=y`` and ``CONFIG_UNICAST_LOCATION_LEFT=y`` or
``CONFIG_UNICAST_LOCATION_RIGHT=y``. This mode allows initiator to set up sink and
source streams simultaneously. ``left.config`` and ``right.config`` project overlay
files in the :zephyr_file:`samples/bluetooth/le_audio/unicast_sink` directory can
be used to configure left and right acceptors.

Sample app supports also Volume Control Service (VCS) and the service can be used
to adjust the volume level of the audio stream(s) in sink direction.

This sample can be tested with an Android, PC or Alif's B1 DevKit.


Building and Running
********************

This sample can be found under :zephyr_file:`samples/bluetooth/le_audio/unicast_sink` in the sdk-alif tree.

See :ref:`Alif bluetooth samples section <alif-bluetooth-samples>` for details.


Limitations
********************

Apple iPhone does not support LE Audio when this sample is published and cannot be used
as an initiator with this sample.

Some Android versions may need a custom application to be able to pair and stream audio
with this sample.

BlueZ version 5.82 or newer is needed to support LE Audio.
