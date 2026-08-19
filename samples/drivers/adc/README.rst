.. zephyr:code-sample:: adc12
   name: Analog-to-Digital Converter (ADC12)
Read analog input from ADC12 channels

Overview
********

This sample app demonstrates the usage of the Analog-to-Digital Conversion (ADC) driver. It displays the temperature based
on the board's temperature, which is converted into an analog signal and fed as input to ADC channel 6.

The temperature will only be displayed if the digital output is in 12-bit format.

Building and Running
********************

The application will build only for a target that has a devicetree entry with :dt compatible:`alif,adc` as a compatible.

.. note::

   - For the ``alif_e1c_dk_rtss_he`` and ``alif_b1_dk_rtss_he`` boards:
     - **ADC12 instance 0**:
       - Channels 0 and 1 pins are multiplexed with ``SE_UART_RX`` and ``SE_UART_TX``. These pins can be used after removing the SE UART jumpers
         and providing analog input to the respective pins after flashing the SE firmware.
       - Channel 2 is connected to the ``OSPI0_SS0`` pin (OSPI chip select) and cannot be used while flashing from the SE tool with OSPI XIP.
     - **ADC12 instance 0 and 1**:
       - Channel 7 is not usable as it is not connected to the internal VREF.

.. code-block:: console

	*** Booting Zephyr OS build ZAS-v1.0.0-rc1-62-g579264343215 ***
	[00:00:00.000,000] <inf> ALIF_ADC: Allocated memory buffer Address is 0x20002050
	[00:00:00.000,000] <inf> ALIF_ADC: Current temp 21.2 C
	[00:00:00.000,000] <inf> ALIF_ADC: ADC sampling Done
