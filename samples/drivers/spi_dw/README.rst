.. _spi-dw-sample:

SPI-DW Sample
####################

Overview
********

This sample demonstrates using the Designware SPI driver.
This sample uses 2 SPI instances one as master and other slave.
By default it uses LPSPI as master and SPI0 as slave.
Master Transmits five 32 bit data and Slave receives the same and prints.

.. note::

Use LPUART port for console logs on E1C and B1 DevKits.

Building and Running
********************

The application will build only for a target that has a devicetree entry with :dt compatible:`snps,designware-spi` as a compatible.

Sample Output
=============

.. minicom output
*** Booting Zephyr OS build zephyr-v3.3.0-112-g274c4710356d ***
slave Receive
Master Transmit
test SPI write only returns: 0
 wrote ffff1111 ffff2222 ffff3333 ffff4444 ffff5555
test SPI read only returns: 5
 red ffff1111 ffff2222 ffff3333 ffff4444 ffff5555
