# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = ["brilliant-ble>=3.1.1,<4"]
# ///
"""
Tests frame.bluetooth.receive_callback(): data sent to the device is handed to
a Lua callback, which echoes it back with frame.bluetooth.send().
"""

import asyncio
import sys
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

    bluetooth = BrilliantBle()
    received = bytearray()

    def on_data(data):
        # The data handler is passed a memoryview, which has no .decode();
        # go via bytes(). Calling .decode() on it raised AttributeError inside
        # the notification handler, so the first data reply killed the run.
        chunk = bytes(data)
        received.extend(chunk)
        print(f"Data: {chunk.decode(errors='replace')}")

    name = await bluetooth.connect(
        name=args.name,
        print_response_handler=lambda string: print(f"Print: {string}"),
        data_response_handler=on_data,
    )
    # Break main.lua before probing: its output otherwise lands in the reply
    # stream and await_print hands it back as the answer to these queries,
    # shifting every field of the banner below by one.
    await bluetooth.send_break_signal()
    fw = await bluetooth.send_lua("print(frame.FIRMWARE_VERSION)", await_print=True)
    tag = await bluetooth.send_lua("print(frame.GIT_TAG)", await_print=True)
    batt = await bluetooth.send_lua("print(frame.battery_level())", await_print=True)
    print(f"{name} | firmware {fw} | git {tag} | battery {batt}%")

    failures = 0
    try:
        # Restart the VM for a clean slate, then break the main.lua it starts:
        # otherwise the app competes for the REPL with the registrations below
        # and they are silently dropped.
        await bluetooth.send_reset_signal()
        await asyncio.sleep(1)
        await bluetooth.send_break_signal()

        await bluetooth.send_lua("function ble_event(d)frame.bluetooth.send(d)end")
        await bluetooth.send_lua("frame.bluetooth.receive_callback(ble_event)")

        # The echo has to be checked while the REPL is idle. The callback is
        # dispatched on the same Lua thread a running chunk occupies, so data
        # sent during a blocking loop is not echoed until that loop ends --
        # which is why this test used to send its payloads underneath a 10 s
        # `frame.sleep` loop and observe nothing.
        for payload in (b"hello there", b"hello"):
            received.clear()
            await bluetooth.send_data(payload)
            for _ in range(50):
                await asyncio.sleep(0.1)
                if bytes(received) == payload:
                    break
            got = bytes(received)
            if got == payload:
                print(f"Passed: echoed {payload!r}")
            else:
                print(f"FAILED: echo of {payload!r} => {got!r}")
                failures += 1

        # Separately: prints stream back while a chunk is running.
        await bluetooth.send_lua("for i=1,5 do print(i); frame.sleep(1) end")
        await asyncio.sleep(7)
    finally:
        try:
            await bluetooth.send_lua("frame.bluetooth.receive_callback(nil)")
        except Exception as e:
            print(f"could not clear receive callback: {e}")
        # Leave the device running its application again.
        await bluetooth.send_reset_signal()
        await bluetooth.disconnect()

    return 1 if failures else 0


sys.exit(asyncio.run(main()))
