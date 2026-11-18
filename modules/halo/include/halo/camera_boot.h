/*
 * Camera sensor reset ordering at boot.
 */
#ifndef HALO_CAMERA_BOOT_H
#define HALO_CAMERA_BOOT_H

/*
 * The camera sensor's reset line is asserted by a kernel init step that
 * runs just before the fixed regulators switch its supplies on, and is
 * released by the camera driver's own reset pulse at its init. Nothing to
 * call: this header exists for documentation and for the init priority
 * check in camera_boot.c.
 */

/**
 * @brief Power the camera down cleanly for the powered-off core state.
 *
 * Asserts the sensor reset and drives the camera rail enable low, then waits
 * for the enable's RC to drain, so both camera supplies are off before the
 * core powers down. Without this the enable pad simply goes dead and the
 * switch's enable floats on its capacitor: the analog supply drops at once
 * while the I/O supply stays up for seconds to minutes, and the sensor
 * wakes from an undefined power state. Call from the deep-sleep path after
 * the suspend chain, right before the power-off.
 */
void halo_camera_sleep_power_down(void);

#endif /* HALO_CAMERA_BOOT_H */
