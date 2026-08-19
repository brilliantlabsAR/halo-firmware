.. zephyr:code-sample:: CRC
        name: Cyclic Redundancy Check (CRC)

###########

Overview
********
This sample application demonstrates the usage of the Cyclic Redundancy Check (CRC)
driver.The application displays the output for the CRC-8-CCITT algorithm. It supports
both aligned and unaligned input data for the CRC.

Building and Running
********************

The application will build only for a target that has a devicetree entry with
:dt compatible:`alif,alif-crc` as a compatible.

console Output
=============
        CRC output: 0xE9
