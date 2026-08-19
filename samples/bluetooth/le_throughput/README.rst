.. _bluetooth-throughput-app:

BLE Throughput
##############

Overview
********

Application to test BLE throughput

Requirements
************

* Alif Balletto Development Kit

Building and Running
********************

This sample can be found under :zephyr_file:`samples/bluetooth/le_throughput` in the
sdk-alif tree.

For testing throughput you need both, central and peripheral devices. That can be configured in
shell by typing either 'tp central' or 'tp peripheral'. After devices have been connected, start test with command
'tp run' in central device shell.

By default, test measures throughput in both directions. First from central to peripheral and then
from peripheral to central. To measure only from central to peripheral, set config
'CONFIG_BLE_TP_BIDIRECTIONAL_TEST' to 'n'.
