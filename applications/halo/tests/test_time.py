# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = ["brilliant-ble>=3.1.1,<4", "luaparser"]
# ///
import asyncio
from brilliant_ble import BrilliantBle
import argparse
from luaparser import ast

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
    fw = await b.send_lua("print(frame.FIRMWARE_VERSION)", await_print=True)
    tag = await b.send_lua("print(frame.GIT_TAG)", await_print=True)
    batt = await b.send_lua("print(frame.battery_level())", await_print=True)
    print(f"{name} | firmware {fw} | git {tag} | battery {batt}%")

    await b.send_lua("print(frame.time.utc())")
    await asyncio.sleep(1.0)

    # Set the current time to Wed Feb 21 2024 21:31:52 UTC
    await b.send_lua("print(frame.time.utc(1708551112))")   
    await asyncio.sleep(5.0)

    print("print(frame.time.utc())")
    await b.send_lua("print(frame.time.utc())")
    await asyncio.sleep(1.0)

    print("frame.time.zone('+04:30') then read back")
    await b.send_lua("frame.time.zone('+04:30')")
    await b.send_lua("print(frame.time.zone())")
    await asyncio.sleep(1.0)

    print("print(frame.time.zone())")
    await b.send_lua("print(frame.time.zone())")
    await asyncio.sleep(1.0)

    print("frame.time.zone('+3:30') then read back")
    await b.send_lua("frame.time.zone('+3:30')")
    await b.send_lua("print(frame.time.zone())")
    await asyncio.sleep(1.0)

    print("print(frame.time.zone())")
    await b.send_lua("print(frame.time.zone())")
    await asyncio.sleep(1.0)

    print("frame.time.zone('5:30') then read back")
    await b.send_lua("frame.time.zone('5:30')")
    await b.send_lua("print(frame.time.zone())")
    await asyncio.sleep(1.0)

    print("print(frame.time.zone())")
    await b.send_lua("print(frame.time.zone())")
    await asyncio.sleep(1.0)

    print("frame.time.zone('-7:30') then read back")
    await b.send_lua("frame.time.zone('-7:30')")
    await b.send_lua("print(frame.time.zone())")
    await asyncio.sleep(1.0)

    print("print(frame.time.zone())")
    await b.send_lua("print(frame.time.zone())")
    await asyncio.sleep(1.0)

    print("print(frame.time.date())")
    await b.send_lua("print(frame.time.date()['year'])")
    await b.send_lua("print(frame.time.date()['month'])")
    await b.send_lua("print(frame.time.date()['day'])")

    await b.send_lua("print(frame.time.date()['hour'])")
    await b.send_lua("print(frame.time.date()['minute'])")
    await b.send_lua("print(frame.time.date()['second'])")

    await b.send_lua("print(frame.time.date()['weekday'])")
    await b.send_lua("print(frame.time.date()['day of year'])")
    await b.send_lua("print(frame.time.date()['is daylight saving'])")
    await asyncio.sleep(1.0)

    # Disconnect Bluetooth
    await b.disconnect()

asyncio.run(main())
