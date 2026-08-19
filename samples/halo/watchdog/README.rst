.. zephyr:code-sample:: watchdog
   name: Watchdog
Use the watchdog driver API to reset the board when it gets stuck in an infinite loop.

Overview
********

This sample demonstrates how to use the watchdog driver API.

A typical use case for a watchdog is that the board is restarted in case some piece of code
is kept in an infinite loop.

Building and Running
********************

This sample can be built and executed on the halo board.

Sample output
=============

You should get a similar output as below:

.. code-block:: console

	Watchdog sample application
	BLE initialized
	Attempting to test pre-reset callback
	Feeding watchdog 5 times
	Feeding watchdog...
	Feeding watchdog...
	Feeding watchdog...
	Feeding watchdog...
	Feeding watchdog...
	Waiting for reset...
	Handled things..ready to reset

.. note:: After the last message, the board will reset and the sequence will start again
