# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = ["brilliant-ble>=3.1.1,<4"]
# ///
"""
Tests the Frame specific Lua libraries over Bluetooth.
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

    # HARDWARE_VERSION is the Zephyr board name on Halo (CONFIG_BOARD, i.e.
    # "halo"), not a Frame-style board revision -- so it does not change between
    # units and is not a hardware identifier.
    await b.send_lua("print('HARDWARE_VERSION (board name): ' .. frame.HARDWARE_VERSION)")
    await b.send_lua("print('FIRMWARE_VERSION: ' .. frame.FIRMWARE_VERSION)")
    await b.send_lua("print('GIT_TAG: ' .. frame.GIT_TAG)")

    # The Secure Enclave revision is the closest thing to a hardware version;
    # it is lazy-loaded rather than exposed as a constant.
    await b.send_lua("print('SE_REVISION: ' .. tostring(frame.get_se_revision()))")

    await asyncio.sleep(1)

    await b.send_reset_signal()
    await b.disconnect()


asyncio.run(main())
