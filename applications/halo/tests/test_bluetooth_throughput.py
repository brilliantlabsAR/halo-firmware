# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = ["brilliant-ble>=3.1.1,<4"]
# ///
"""
Measures device -> host BLE throughput.

The device sends a batch of full-MTU payloads and then a one-byte sentinel to
close the measurement window. The sentinel is deliberately NOT the empty string
the original version used: frame.bluetooth.send() chunks with
`while (remaining > 0)` (lua_bluetooth.c), so a zero-length payload transmits
nothing and returns success. The host would wait for a marker that was never
put on the wire, which is one of the two reasons this test measured nothing.

The other was that the payload script was commented out entirely, so the run
connected, printed the battery level and exited without transferring anything --
while still "passing", because nothing checked.

The loop runs from the REPL rather than being uploaded as main.lua. That keeps
the device's own application untouched: a break signal stops it, and there is
nothing to restore afterwards.
"""

import argparse
import asyncio
import sys
import time

from brilliant_ble import BrilliantBle

SENTINEL = b"\x00"
BATCH = 100


class Throughput:
    def __init__(self):
        self.bytes_in_window = 0
        self.window_started = None
        self.samples = []

    def on_data(self, data):
        chunk = bytes(data)
        if chunk == SENTINEL:
            if self.window_started is not None and self.bytes_in_window:
                elapsed = time.monotonic() - self.window_started
                if elapsed > 0:
                    kbps = self.bytes_in_window / elapsed / 1000
                    self.samples.append(kbps)
                    print(f"Throughput: {kbps:.2f} KB/s "
                          f"({self.bytes_in_window} bytes in {elapsed:.2f}s)")
            self.bytes_in_window = 0
            self.window_started = time.monotonic()
            return
        if self.window_started is None:
            self.window_started = time.monotonic()
        self.bytes_in_window += len(chunk)


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
        default=20.0,
        help="seconds to measure for (default: 20)",
    )
    args = parser.parse_args()

    tp = Throughput()
    b = BrilliantBle()

    name = await b.connect(name=args.name, data_response_handler=tp.on_data)
    # Break main.lua before probing: its output otherwise lands in the reply
    # stream and await_print hands it back as the answer to these queries.
    await b.send_break_signal()
    fw = await b.send_lua("print(frame.FIRMWARE_VERSION)", await_print=True)
    tag = await b.send_lua("print(frame.GIT_TAG)", await_print=True)
    batt = await b.send_lua("print(frame.battery_level())", await_print=True)
    print(f"{name} | firmware {fw} | git {tag} | battery {batt}%")

    try:
        mtu = await b.send_lua("print(frame.bluetooth.max_length())", await_print=True)
        print(f"payload size {mtu} bytes, {BATCH} per window, measuring "
              f"{args.duration:g}s")

        # Sent as separate statements so each stays inside max_lua_payload().
        await b.send_lua(
            "function _tp_send(d) while true do "
            "if pcall(frame.bluetooth.send, d) then break end end end"
        )
        await b.send_lua("_tp_data = string.rep('a', frame.bluetooth.max_length())")
        # Runs until interrupted; the break signal below stops it.
        await b.send_lua(
            f"while true do for i = 1, {BATCH} do _tp_send(_tp_data) end "
            "_tp_send('\\0') end"
        )

        await asyncio.sleep(args.duration)
    finally:
        await b.send_break_signal()
        await asyncio.sleep(1)

    if tp.samples:
        best = max(tp.samples)
        mean = sum(tp.samples) / len(tp.samples)
        print(f"\n{len(tp.samples)} windows | mean {mean:.2f} KB/s | peak {best:.2f} KB/s")
    else:
        print("\nFAILED: no throughput windows completed - no data was received")

    # Leave the device running its application again.
    await b.send_reset_signal()
    await b.disconnect()
    return 0 if tp.samples else 1


sys.exit(asyncio.run(main()))
