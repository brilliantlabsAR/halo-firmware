.. zephyr:code-sample:: alif_ble_smp_svr
   :name: SMP server using Alif BLE stack

   Use Alif BLE stack to implement a custom Simple Management Protocol (SMP) service for
   Device Firmware Update (DFU).

Overview
********

Application demonstrating the usage of Alif BLE stack for implementing a custom SMP service.

The sample will start advertising the SMP service using BLE and waits for a single client to
connect. After connection, the client can perform firmware updates using the Simple Management
Protocol.

Requirements
************

* Alif Balletto Development Kit
* Android or iOS device with the Alif application or
* Mcumgr command-line tool running on Linux or macOS

Building and Running
********************

Building the MCUboot bootloader and the sample SMP application can be done in a single command
using sysbuild. Sysbuild will build the bootloader and the application. The application is
automatically built to support booting with MCUboot. Application signing is also done by sysbuild.

.. code-block:: console

   west build --sysbuild -b alif_b1_dk_rtss_he alif/samples/bluetooth/smp_svr

The resulting binaries then need to be flashed to the correct addresses in MRAM. The MCUboot
bootloader uses fixed addresses for image slots.

.. code-block:: console
   Image                                     MRAM address
   build/mcuboot/zephyr/zephyr.bin           0x80000000
   build/smp_svr/zephyr/zephyr.signed.bin    0x80010000

Flashing the Binaries
*********************

Use the Alif SETools to flash the bootloader and the signed user application.


Copy the bootloader and signed user application from the build directory:

.. code-block:: console

   <ZEPHYR_ROOT>/build/mcuboot/zephyr/zephyr.bin
   <ZEPHYR_ROOT>/build/smp_svr/zephyr/zephyr.signed.bin

To your SETools build directory:

.. code-block:: console

   <SETOOLS_ROOT>/build/images/zephyr.bin
   <SETOOLS_ROOT>/build/images/zephyr.signed.bin

Create SETools configuration file in:

.. code-block:: console

   <SETOOLS_ROOT>/build/config/balletto_smp_svr.json

With the following contents:

.. code-block:: console

   {
      "DEVICE": {
         "disabled" : false,
         "binary": "app-device-config.json",
         "version" : "0.5.00",
         "signed": true
      },
      "mcuboot": {
         "disabled": false,
         "binary": "zephyr.bin",
         "version" : "1.0.0",
         "signed": true,
         "mramAddress": "0x80000000",
         "cpu_id": "M55_HE",
         "flags": ["boot"]
      },
      "smp_svr": {
         "disabled": false,
         "binary": "zephyr.signed.bin",
         "version": "1.0.0",
         "signed": false,
         "mramAddress": "0x80010000",
         "cpu_id": "M55_HE",
         "flags": []
      }
   }

Finally use SETools to flash the images:

.. code-block:: console

   app-gen-toc -f build/config/balletto_smp_svr.json
   app-write-mram -p


Updating the Image Using the mcumgr Tool
****************************************

The mcumgr command-line tool supports BLE only on Linux and macOS.

1. Upload the new firmware image:

   .. code-block:: console

      mcumgr -conntype ble --connstring ctlr_name=hci0,peer_name='ALIF_SMP' \
         image upload build/smp_svr/zephyr/zephyr.signed.bin

2. List the available images to confirm the upload:

   .. code-block:: console

      mcumgr -conntype ble --connstring ctlr_name=hci0,peer_name='ALIF_SMP' \
         image list

3. Test the new image:

   .. code-block:: console

      mcumgr -conntype ble --connstring ctlr_name=hci0,peer_name='ALIF_SMP' \
         image test <slot1_image_hash>

4. Reset the device to test the new image:

   .. code-block:: console

      mcumgr -conntype ble --connstring ctlr_name=hci0,peer_name='ALIF_SMP' \
         reset

5. Confirm the new image if it works as expected:

   .. code-block:: console

      mcumgr -conntype ble --connstring ctlr_name=hci0,peer_name='ALIF_SMP' \
         image confirm <slot1_image_hash>

Updating the Image Using the Alif mobile application
****************************************************

1. Scan and connect to the device.

2. Inspect the current hash in the info tab.

3. Build a new zephyr.signed.bin binary with the same command and export it
      to the mobile phone or to a cloud service. The new file will have a
      different hash.

4. Load the new file from the Upgrade tab in the mobile phone.

5. Tap the update button and wait until the new firmware is uploaded.

6. On Android, the app will reconnect to the device after restart and the new
      hash can be seen in the info tab.
      On iOS, the app will disconnect after the update. Connect again to
      inspect the new hash.
