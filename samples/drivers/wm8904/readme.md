## WM8904 sample

This sample app demonstrates the usage of the WM8904 audio codec, by sending audio data to it over I2S. The audio data sent is a constant frequency tone, stereo, in 16-bit data width.

At the moment the codec driver is a minimal implementation optimised for the specific LE audio application settings used on the Balletto FPGA board (48 kHz sampling, 1.536 MHZ MCLK). But could be extended in future with further configuration options.

It is important to be aware of the following when building and using the app:
- An SI570 oscillator is used to provide the MCLK signal for the I2S. If the SI570 is not set up, no data will be clocked out over I2S.
- The Ensemble I2S driver is used. This currently requires the patch shown below to be applied to the Zephyr repo, to make this driver available for the Balletto SOC.

```
diff --git a/drivers/i2s/Kconfig.ensemble b/drivers/i2s/Kconfig.ensemble
index 5ad7c3685b..4e8becc7bd 100644
--- a/drivers/i2s/Kconfig.ensemble
+++ b/drivers/i2s/Kconfig.ensemble
@@ -6,7 +6,7 @@ menuconfig I2S_ENSEMBLE
        bool "ENSEMBLE MCU I2S controller driver"
        default y
        #depends on DT_HAS_ALIF_ENSEMBLE_I2S_ENABLED
-       depends on SOC_FAMILY_ENSEMBLE
+       depends on SOC_FAMILY_ENSEMBLE || SOC_FAMILY_BALLETTO
        #select DMA
        help
          Enable I2S support on the Alif Ensemble family of processors.
```
