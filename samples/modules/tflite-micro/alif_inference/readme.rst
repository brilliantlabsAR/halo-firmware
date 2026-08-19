.. _tflite-micro-alif-inference-sample:

Keyword Spotting using Generic Inference Runner
###############################################

Overview
********

This sample demonstrates how to use a generic inference runner to perform Keyword Spotting (KWS) on Alif devices.

Requirements
************

- Alif Ensemble or Balletto Development Kit

Building and Running
********************

This sample is located at :zephyr_file:`samples/modules/tflite-micro/alif_kws` in the sdk-alif tree.

To build the sample, you first need to pull in the optional dependencies by running the following commands:

.. code-block:: console

   west config manifest.group-filter -- +optional
   west update
