.. _bluetooth-broadcast-sink-sample:

BLE Audio Broadcast Sink Sample
###############################

Overview
********

This sample demonstrates the LE audio broadcast sink use-case.

It scans for broadcast sources and synchronises with the first compatible source found.
The audio configuration (sampling rate, frame duration) is dynamically determined from the broadcast source and used to configure the audio codec at runtime.

Both stereo and mono sources are supported. In case of a mono source, the single channel will be
duplicated on both left and right I2S output streams.

Building and Running
********************

This sample can be found under :zephyr_file:`samples/bluetooth/broadcast_sink` in the
sdk-alif tree.

See :ref:`Alif bluetooth samples section <alif-bluetooth-samples>` for details.

BAP defined Codec Configuration Settings (Dynamically Supported)
***************************************************************

.. table:: BAP defined Codec Configuration Settings
   :widths: 1 1 1 1 1

   +---------------+-----------+-----------+-------------+---------+
   | Codec         | Supported | Supported | Supported   | Bitrate |
   | Configuration | Sampling  | Frame     | Octets per  | (kbps)  |
   | Setting       | Frequency | Duration  | Codec Frame |         |
   +===============+===========+===========+=============+=========+
   | 8_1           | 8         | 7.5 ms    | 26          | 27.734  |
   | 8_2           | 8         | 10 ms     | 30          | 24      |
   | 16_1          | 16        | 7.5 ms    | 30          | 32      |
   | 16_2\*        | 16        | 10 ms     | 40          | 32      |
   | 24_1          | 24        | 7.5 ms    | 45          | 48      |
   | 24_2\*\*      | 24        | 10 ms     | 60          | 48      |
   | 32_1          | 32        | 7.5 ms    | 60          | 64      |
   | 32_2          | 32        | 10 ms     | 80          | 64      |
   | 441_1         | 44.1      | 7.5 ms    | 97          | 95.06   |
   | 441_2         | 44.1      | 10 ms     | 130         | 95.55   |
   | 48_1          | 48        | 7.5 ms    | 75          | 80      |
   | 48_2          | 48        | 10 ms     | 100         | 80      |
   | 48_3          | 48        | 7.5 ms    | 90          | 96      |
   | 48_4          | 48        | 10 ms     | 120         | 96      |
   | 48_5          | 48        | 7.5 ms    | 117         | 124.8   |
   | 48_6          | 48        | 10 ms     | 155         | 124     |
   +---------------+-----------+-----------+-------------+---------+

\*\* Auracast mandated Standard Quality Public Broadcast Codec Configuration option 1
\*\* Auracast mandated Standard Quality Public Broadcast Codec Configuration option 2

All Auracast transmitters must support at least one broadcast stream that uses one of the
Standard Quality(SQ) codec configurations which are defined in Public Boradcast Profile(PBP).
Support for other codec configurations is optional.
Low latency configurations set retransmissions to 2, the high reliability ones to 4.
Low latency configurations are beneficial when there is ambient sound which would be causing echo effects.

