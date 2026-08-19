.. _alif-bluetooth-samples:

Alif Bluetooth samples
######################

To build any of the Alif Bluetooth samples, follow the same steps as building
any other Zephyr application, See :ref:`bluetooth samples section <bluetooth-samples>` for details.
The difference for the normal Zephyr Bluetooth application is that you need to have BT_CUSTOM
enabled but this is already taken into account in the sample applications present in Alif SDK.
Refer to :ref:`bluetooth-dev` for more information.

For all the sample applications in Alif SDK you need to have Alif Balletto family B1 board.
Alif Host stack and LC3 codec resides in ROM so you will have more memory for your application
compared to Zephyr host stack based samples where BLE host stack would reside in RAM.


.. toctree::
   :maxdepth: 1
   :glob:

   **/*
