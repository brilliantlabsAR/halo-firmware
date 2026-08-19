.. _lpi2c-sample:

LPI2C Sample
####################

Overview
********

This sample demonstrates using the alif LPI2C driver.
This sample uses I2C instances one as master and the alif lpi2c as slave.
By default it uses i2c0 as master and lpi2c as slave.

Building and Running
********************

The application will build only for a target that has a devicetree entry with
:dt compatible:`alif,lpi2c` as a compatible.

NOTE
============

when lpi2c transmit 3 bytes then master reception has to be called 3 times
to receive the 3 bytes send by the slave.

Sample Output
=============
Welcome to minicom 2.7.1

OPTIONS: I18n
Compiled on Dec 23 2019, 02:06:26.
Port /dev/ttyACM1, 12:41:44

Press CTRL-A Z for help on special keys

*** Booting Zephyr OS build Zephyr-Alif-SDK-v0.5.0-17-g17b360353343 ***
[00:00:00.000,000] <inf> ALIF_LPI2C: Start Master trasmit and Slave receive
[00:00:00.001,000] <inf> ALIF_LPI2C: Master transmit and slave receive successful
[00:00:00.002,000] <inf> ALIF_LPI2C: Start Slave transmit and Master receive
[00:00:00.006,000] <inf> ALIF_LPI2C: Slave transmit and Master receive successful
[00:00:00.006,000] <inf> ALIF_LPI2C: Transfer completed

