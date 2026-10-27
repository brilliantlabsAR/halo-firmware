/*
 * Serializes power sequences that glitch a shared I2C bus against the
 * transactions of other devices on that bus which cannot tolerate the glitch.
 *
 * On Halo the TPS65132 display bias supply shares I2C1 with the PAG7982 camera
 * sensor. Powering the TPS65132 up (or starting one of its outputs) can corrupt
 * a transfer in flight and leave the controller waiting for a STOP that never
 * comes; its driver recovers the bus afterwards, but a sensor register access
 * that straddles the glitch fails anyway, and the sensor's own retries fall
 * before the recovery. The TPS65132 driver holds this guard from the moment it
 * powers the chip until the bus is recovered; the camera driver holds it across
 * the sensor's reset-and-configure sequence. The lock is recursive for its
 * owner, so a driver may take it around a sequence that itself takes it.
 */

#ifndef HALO_DRIVERS_I2C_GLITCH_GUARD_H_
#define HALO_DRIVERS_I2C_GLITCH_GUARD_H_

#ifdef __cplusplus
extern "C" {
#endif

/** Take the guard; blocks while another thread holds it. */
void i2c_glitch_guard_lock(void);

/** Release the guard taken by i2c_glitch_guard_lock(). */
void i2c_glitch_guard_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* HALO_DRIVERS_I2C_GLITCH_GUARD_H_ */
