.. zephyr:code-sample:: DAC
		name: Digital-to-Analog Converter (DAC)

###########

Overview
********
This sample application demonstrates the usage of the Digital-to-Analog Converter (DAC)
driver.The application converts 12-bit digital values into analog voltage signals.

Building and Running
********************

The application will build only for a target that has a devicetree entry with
:dt compatible:`alif,dac` as a compatible.

.. code-block:: console

    *** Booting Zephyr OS build ZAS-v1.0.0-rc1-47-g119112f8f92b ***

     >>>Starting up the Zephyr DAC demo!!! <<<

