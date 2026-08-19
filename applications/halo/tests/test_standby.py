# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = ["brilliant-ble>=3.1.1,<4", "aioconsole"]
# ///
"""
Tests frame.standby()'s resume-in-place wake semantics.

An infinite counting loop enters standby every 5 counts. On wake, execution
continues with the statement after the standby() call: the "Woke up!" line
prints (with frame.wakeup_source()) and the counter continues from where it
was, rather than restarting. Contrast test_light_sleep.py: light sleep
restarts the VM, so main.lua runs again from the top.
"""

import asyncio
from brilliant_ble import BrilliantBle
import argparse
from aioconsole import ainput


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
    print("Connecting to Frame device...")
    name = await b.connect(name=args.name, print_response_handler=lambda s: print(s))
    fw = await b.send_lua("print(frame.FIRMWARE_VERSION)", await_print=True)
    tag = await b.send_lua("print(frame.GIT_TAG)", await_print=True)
    batt = await b.send_lua("print(frame.battery_level())", await_print=True)
    print(f"{name} | firmware {fw} | git {tag} | battery {batt}%")
    
    await b.send_break_signal()  # Ensure no script is running

    print("=== Testing standby resume-in-place ===")

    lua_script = "count = 0 while true do count = count + 1 print('Count: ' .. count) frame.yield() if count % 5 == 0 then print('Count reached ' .. count .. ', entering standby...') frame.standby(5) print('Woke up (' .. frame.wakeup_source() .. ')! Continuing count...') else frame.sleep(0.5) end end"

    await b.send_lua(lua_script)

    print("\n   Device should be counting and will standby at count=5...")
    print("   Wake it up with button, and it will continue counting...")

    print("\nPress Enter to send BREAK signal to interrupt the script...\n")
    await ainput()  # Wait for user to press Enter

    print("Sending interrupt signal (equivalent to Ctrl+C)...")
    await b.send_break_signal()

    await b.disconnect()


asyncio.run(main())
