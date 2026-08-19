.. _tflite-micro-alif-kws-sample:

Machine Learning Keyword Spotting Sample
########################################

Overview
********

This sample is a Zephyr port of the `Alif ML Embedded Evaluation Kit <https://github.com/alifsemi/alif_ml-embedded-evaluation-kit>`_
for the keyword spotting (KWS) use case.

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
