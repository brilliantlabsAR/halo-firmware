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

#endif /* HALO_CAMERA_BOOT_H */
