.. _i2c-dw-sample:

I2C-DW Sample
####################

Overview
********

This sample demonstrates using the Designware I2C driver.
This sample uses 2 I2C instances one as master and the other as slave.
By default it uses i2c0 as master and i2c1 as slave.

Building and Running
********************

The application will build only for a target that has a devicetree entry with :dt compatible:`snps,designware-i2c` as a compatible.

Sample Output
=============

.. minicom output
*** Booting Zephyr OS build zephyr-v3.3.0-120-gd948f1171dd4 ***
Received a byte in slave : 0xaa
Received a byte in slave : 0xab
Received a byte in slave : 0xac
Received a byte in slave : 0xad
Master wrote 0xaa 0xab 0xac 0xad to slave
Read requested from Master and send 0x60 from slave
Read processed_cb called
Master received Data 0x60 From Slave
