## Alif B1 FPGA board

On this board, audio support relies on the following additional hardware:
- SI570 clock generator, to generate the MCLK signal. This is connected via a TCA9548 I2C mux.
- WM8904 audio codec, to record audio data and send to the SOC via I2S.
