.. _video-sample:

Video Sample
#############################################

Overview
********

This sample can be used to capture frame using Parallel-bus or CSI camera sensors, and store it to
the memory. The functional behaviour is:

* Camera sensor will send out frames along with synchronisation signals either to CAM/LPCAM
  controller directly (in case of the parallel camera sensor eg. MT9M114), or to Camera Serial
  Interface MIPI CSI2 and then to CAM controller (in case of serial camera sensor eg. ARX3A0).
* MIPI CSI2 uses DPHY interface to receive data serially from serial camera sensor and convert it
  to parallel data since CAM/LPCAM controller has parallel interface.
* CAM/LPCAM controller will convert the captured frame with desired features and save it into the
  memory.

Requirements
************

The sample utilizes the CAM Controller IP from alif and a camera sensor. It may also use the
MIPI-CSI2 IP from Synopsys if the serial camera sensor application is built from the command line.
The camera sensors used in the parallel camera case is the MT9M114, while serial camera include
ARX3A0 camera sensor.

Supported Targets
*****************

* alif_e7_dk_rtss_hp
* alif_e7_dk_rtss_he
* alif_e1c_dk_rtss_he
* alif_b1_dk_rtss_he

Tested Sensors
**************

* MT9M114 (Parallel)
* ARX3A0 (CSI)
* HM0360 (CSI)

Sample Output
*************

.. code-block:: console

  *** Booting Zephyr OS build zas-v1.1-main-4-gf29b5249b50b ***
  - Device name: cam@49030000
  - Capabilities:
    Y10P width (min, max, step)[560; 560; 0] height (min, max, step)[560; 560; 0]
  - format: Y10P 560x560
  Width - 560, Pitch - 560, Height - 560, Buff size - 313600
  - addr - 0x8000000, size - 313600, bytesused - 0
  capture buffer[0]: dump binary memory "/home/$USER/path/capture_0.bin" 0x08000000 0x0804c8ff -r

  - addr - 0x804c900, size - 313600, bytesused - 0
  capture buffer[1]: dump binary memory "/home/$USER/path/capture_1.bin" 0x0804c900 0x080991ff -r

  Capture started
  Got frame 0! size: 313600; timestamp 8283 ms
  Got frame 1! size: 313600; timestamp 8483 ms
  Got frame 2! size: 313600; timestamp 8683 ms
  Got frame 3! size: 313600; timestamp 8883 ms
  Got frame 4! size: 313600; timestamp 9083 ms
  Got frame 5! size: 313600; timestamp 9283 ms
  Got frame 6! size: 313600; timestamp 9483 ms
  Got frame 7! size: 313600; timestamp 9683 ms
  Got frame 8! size: 313600; timestamp 9883 ms
  Got frame 9! size: 313600; timestamp 10083 ms
  [00:00:10.083,000] <inf> video_app: Calling video flush.
  [00:00:10.083,000] <inf> video_app: Calling video stream stop.
