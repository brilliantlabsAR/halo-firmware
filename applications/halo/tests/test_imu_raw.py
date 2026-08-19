# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = ["brilliant-ble>=3.1.1,<4"]
# ///
"""
Polls frame.imu.raw() and prints the accelerometer and compass readings.

Columns are the RAW DEVICE AXES. That is not the frame frame.imu.direction()
reports: it remaps to a host (right, forward, up) convention before computing
tilt, as host_X = -dev.z, host_Y = dev.y, host_Z = dev.x (see lua_imu.c). So
which column carries gravity depends on how the device is oriented in its own
frame, and comparing these numbers against direction() -- or against the Frame
convention -- will mislead unless the remap is applied first.

Do not calibrate against a dev kit: its IMU and magnetometer are mounted
differently from a production unit, so the axis carrying gravity in a given
physical pose is not the same on the two. Both lying flat on a table:

    dev kit:               ax ~   17, ay ~ -45, az ~ +1007   (gravity on dev z)
    production unit:       ax ~ +981, ay ~  +8, az ~   +11   (gravity on dev x)

The remap direction() applies targets the production mounting, so a production
unit lying flat reports level (roll/pitch ~ 0) while a dev kit in the same pose
reports roll ~ -89. That is expected, not a fault.
"""

import asyncio
from brilliant_ble import BrilliantBle
import argparse


async def main():
    parser = argparse.ArgumentParser(
        description="Connect to a Halo/Frame device over BLE and run this test."
    )
    parser.add_argument(
        "--name",
        default=None,
        help='exact BLE device name, e.g. "Halo AB" or "Frame 4F"; defaults to the nearest device',
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=30.0,
        help="seconds to poll for; 0 polls until interrupted (default: 30)",
    )
    args = parser.parse_args()

    b = BrilliantBle()

    name = await b.connect(name=args.name, print_response_handler=lambda s: print(s))
    # Break main.lua before probing: its output otherwise lands in the reply
    # stream and await_print hands it back as the answer to these queries,
    # shifting every field of the banner below by one.
    await b.send_break_signal()
    fw = await b.send_lua("print(frame.FIRMWARE_VERSION)", await_print=True)
    tag = await b.send_lua("print(frame.GIT_TAG)", await_print=True)
    batt = await b.send_lua("print(frame.battery_level())", await_print=True)
    print(f"{name} | firmware {fw} | git {tag} | battery {batt}%")

    try:
        # Enable taps. DOUBLE tap only - tap_callback arms SENSOR_TRIG_DOUBLE_TAP.
        await b.send_lua("frame.imu.tap_callback((function()print('Tap!')end))")

        # Header once, not once per sample.
        print("device-frame accelerometer\t\t\tdevice-frame compass")
        print("ax\tay\taz\t\t\t\tcx\tcy\tcz")

        loop = asyncio.get_running_loop()
        deadline = None if args.duration <= 0 else loop.time() + args.duration
        while deadline is None or loop.time() < deadline:
            await b.send_lua("resp = frame.imu.raw()")
            await b.send_lua(
                "print(tostring(resp['accelerometer']['x'])..'\t'..tostring(resp['accelerometer']['y'])..'\t'..tostring(resp['accelerometer']['z'])..'\t'..tostring(resp['compass']['x'])..'\t'..tostring(resp['compass']['y'])..'\t'..tostring(resp['compass']['z']))",
                await_print=True,
            )
            await asyncio.sleep(0.1)
    finally:
        # Disarm the tap trigger. Leaving it armed keeps LPGPIO0 asserting and
        # changes the behaviour of anything run afterwards.
        try:
            await b.send_lua("frame.imu.tap_callback(nil)")
        except Exception as e:
            print(f"could not clear tap callback: {e}")
        await b.send_reset_signal()
        await b.disconnect()


asyncio.run(main())
