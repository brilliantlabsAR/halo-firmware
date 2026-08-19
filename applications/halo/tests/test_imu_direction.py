# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = ["brilliant-ble>=3.1.1,<4"]
# ///
"""
Polls frame.imu.direction() and prints roll, pitch and heading.

roll and pitch are computed in the host (right, forward, up) frame, so a device
held level reads ~0 for both -- but only on production hardware. The remap
targets the production IMU mounting, and a dev kit's die sits differently, so:

    production unit flat on a table:       roll ~ -0.8, pitch ~ +0.8  (level)
    dev kit flat on a table:               roll ~ -89                 (not level)

The dev-kit reading is expected, not a fault. Check anything orientation
related on real hardware.

heading is NOT computed: lua_imu.c assigns it a literal 0.0 pending
magnetometer support, so the column is always 0 (#252).
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

    print("note: heading is a firmware stub and always reads 0.0 (#252)")

    try:
        # Enable taps. DOUBLE tap only - tap_callback arms SENSOR_TRIG_DOUBLE_TAP.
        await b.send_lua("frame.imu.tap_callback((function()print('Tap!')end))")

        loop = asyncio.get_running_loop()
        deadline = None if args.duration <= 0 else loop.time() + args.duration
        while deadline is None or loop.time() < deadline:
            await b.send_lua("resp = frame.imu.direction()")
            await b.send_lua(
                "print('roll: '..tostring(resp['roll'])..'\tpitch: '..tostring(resp['pitch'])..'\theading: '..tostring(resp['heading']))",
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
