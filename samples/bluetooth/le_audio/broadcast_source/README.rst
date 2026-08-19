.. _bluetooth-broadcast-source-sample:

BLE Audio Broadcast Source Sample
#################################

Overview
********

This sample demonstrates the LE audio broadcast source use-case.

The broadcast name used can be configured using `CONFIG_BROADCAST_NAME`.
This might need to be changed for compatibility with a 3rd party broadcast sink device, as the broadcast name is sometimes used to choose which broadcast source to receive from.

Push-to-talk feature is also included in this sample. Push-to-talk feature is enabled by default.
This means that input audio gain will be lowered and MIC input mixed into audio samples when the devkit's joystick button is pressed.
This feature can be disabled by removing  `i2s-mic = &i2s2;` from the board's devicetree overlay file.

Building and Running
********************

This sample can be found under :zephyr_file:`samples/bluetooth/broadcast_source` in the
sdk-alif tree.

See :ref:`Alif bluetooth samples section <alif-bluetooth-samples>` for details.

Configuration options:

.. list-table::

    * - :file:`prj.conf`
      - This is the standard default config. Sampling frequency 48kHz, 10ms frame duration, 100 octets per codec frame. The default retransmissions are set to 4.

    * - :file:`overlay-auracast_16_2.conf`
      - Enables a Standard Quality codec configuration. 16-bit sample rate, 40 octets per codec frame.

    * - :file:`overlay-auracast_24_2.conf`
      - Enables a Standard Quality codec configuration. 24-bit sample rate, 60 octets per codec frame.

BAP defined Codec Configuration Settings
******************************************

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
