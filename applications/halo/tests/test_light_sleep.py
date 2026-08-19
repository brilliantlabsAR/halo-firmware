# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = ["brilliant-ble>=3.1.1,<4"]
# ///
"""
Tests frame.light_sleep()'s wake semantics.

Halo's two application sleep modes wake differently, by design:
  * frame.standby() resumes in place -- execution continues with the
    statement after the call;
  * frame.light_sleep() restarts the VM -- on wake the PM callback's break
    hook throws "interrupted", that propagates out of require('main'), and
    the REPL outer loop restarts the VM, so main.lua runs again from the top
    (lua_runtime.c). frame.wakeup_source(), read at the top of the script,
    is how a script learns why it is running after a wake.

This test asserts the light-sleep (restart) semantics:
  * the script's banner appears again after the sleep (the VM restarted);
  * the counter starts over rather than continuing;
  * the resume-in-place marker never appears.

test_standby.py covers the resume-in-place mode.
"""

import argparse
import asyncio
import sys

from brilliant_ble import BrilliantBle
from halo_device_file import preserve_main_lua

BANNER = "LSTEST-BOOT"
RESUMED = "LSTEST-RESUMED-IN-PLACE"
SLEEP_AT = 3
SLEEP_SECS = 3

# One line: a newline would terminate the REPL command early.
LUA_SCRIPT = (
    f"print('{BANNER} src=' .. tostring(frame.wakeup_source())) "
    "count = 0 "
    "while true do "
    "count = count + 1 "
    "print('Count: ' .. count) "
    f"if count == {SLEEP_AT} then "
    f"frame.light_sleep({SLEEP_SECS}) "
    f"print('{RESUMED}') "
    "end "
    "frame.sleep(1) "
    "end"
)


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
        default=25.0,
        help="seconds to observe for; needs to span at least one sleep/wake (default: 25)",
    )
    args = parser.parse_args()

    output = []

    def on_print(s):
        output.append(s)
        print(f"[Device output] {s}", end="" if s.endswith("\n") else "\n")

    b = BrilliantBle()
    name = await b.connect(name=args.name, print_response_handler=on_print)
    await b.send_break_signal()
    fw = await b.send_lua("print(frame.FIRMWARE_VERSION)", await_print=True)
    batt = await b.send_lua("print(frame.battery_level())", await_print=True)
    print(f"{name} | firmware {fw} | battery {batt}%")

    failures = 0
    # light_sleep restarts main.lua, so the script has to BE main.lua. Keep the
    # device's own copy and put it back.
    async with preserve_main_lua(b):
        await b.upload_file_from_string(LUA_SCRIPT, "main.lua")
        output.clear()
        await b.send_reset_signal()

        print(f"observing {args.duration:g}s (sleeps {SLEEP_SECS}s at count {SLEEP_AT})")
        await asyncio.sleep(args.duration)
        await b.send_break_signal()

        text = "\n".join(output)
        boots = text.count(BANNER)
        counts = [int(l.split("Count: ")[1]) for l in output if "Count: " in l]

        # The VM should have restarted, so the banner appears more than once.
        if boots >= 2:
            print(f"Passed: main.lua restarted after light_sleep ({boots} banners)")
        else:
            print(f"FAILED: expected >= 2 banners (VM restart), saw {boots}")
            failures += 1

        # Execution must NOT continue after the light_sleep() call.
        if RESUMED not in text:
            print("Passed: execution did not resume in place, as expected on Halo")
        else:
            print(f"FAILED: {RESUMED} printed - resume-in-place semantics changed")
            failures += 1

        # The counter should start over rather than exceed the sleep threshold.
        if counts and max(counts) <= SLEEP_AT:
            print(f"Passed: counter restarted (max {max(counts)} <= {SLEEP_AT})")
        else:
            print(f"FAILED: counter reached {max(counts) if counts else 'nothing'}, "
                  f"expected <= {SLEEP_AT}")
            failures += 1

    await b.disconnect()
    return 1 if failures else 0


sys.exit(asyncio.run(main()))
