## I2S sync sample

This sample app demonstrates the usage of the I2S sync driver. This driver provides a callback-based approach to I2S, which allows the application finer control over the timing/synchronisation of the I2S data, since timing information can be obtained in the callback called every time a block (RX or TX) is completed.

The demo on the `alif_b1_fpga_ble_rtss_he` board also relies on:
- SI570 for generation of the MCLK signal
- WM8904 to receive the audio data over I2S and output it to a headphone jack.

The application uses the I2S in a "loopback" configuration to demonstrate both TX and RX directions. The WM8904 codec samples analog audio data received on it's line-in input. This is received by the application over I2S. The same data is then sent back out on the I2S TX channel, and the WM8904 codec converts back to an analog audio signal on the headphone jack. The audio is stopped and started periodically to demonstrate disabling of the I2S interface.

To use the application, plug some headphones into the headphone jack of the WM8904 board, and use the line-in jack to send audio to the codec from e.g. a phone.
