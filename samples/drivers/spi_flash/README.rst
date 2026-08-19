
.. _spi-flash-test:

SPI-Flash Test
###############

Overview
********

This is the test application to test and verify the Flash Read,Write and Erase opearations over OSPI interface.
The OSPI driver placed under *modules/hal/alif/drivers*
As of current h/w support 16bit Data Frame Size been applied for R/W.


Building and Running
********************

The application will build only for a target that has a devicetree entry with *:dt compatible:`snps,designware-ospi`* as a compatible.

Sample Output
=============

.. code-block:: console

	ospi1@83002000 OSPI flash testing
	========================================

	Test 1: Flash erase
	Flash erase succeeded!

	Test 1: Flash write
	Attempting to write 4 bytes

	Test 1: Flash read
	Data read matches data written. Good!!

	Test 2: Flash Full Erase
	Successfully Erased whole Flash Memory
	Total errors after reading erased chip = 0

	Test 3: Flash erase
	Flash erase succeeded!

	Test 3: Flash write
	Attempting to write 1024 bytes

	Test 3: Flash read
	Data read matches data written. Good!!

	Test 4: write sector 16384
	Test 4: write sector 20480

	Sec4: Read and Verify written data

	Test 4: read sector 16384

	Data read matches data written. Good!!
	Sec5: Read and Verify written data

	Test 4: read sector 20480
	Data read matches data written. Good!!

	Test 4: Erase Sector 4 and 5
	Flash Erase from Sector 16384 Size to Erase 8192

	Multi-Sector erase succeeded!

	Test 4: read sector 16384
	Total errors after reading erased Sector 4 = 0

	Test 4: read sector 20480
	Total errors after reading erased Sector 5 = 0

	Multi-Sector Erase Test Succeeded !

