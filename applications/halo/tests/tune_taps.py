# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = ["brilliant-ble>=3.1.1,<4"]
# ///
"""
Interactive tap-detector tuning console for Halo (BMA580).

Arms all three tap gestures, prints every event with a host-side timestamp,
and lets you adjust the detector configuration live over the REPL:

    uv run tune_taps.py --name "Halo AB"

Commands (at the "tune>" prompt):
    get                      show current tap config
    mode sensitive|normal|robust
    axis x|y|z               dominant sensing axis (single axis only!)
    threshold N              peak threshold, 0..1023 (chip default: 24;
                             Bosch hearable default: 750)
    max_peaks N              0..7
    gesture_duration N       0..63 (window for 2nd/3rd tap)
    wait_for_timeout 0|1     1 = single confirmed only after window expires
    peak_duration N          0..15
    shock_duration N         0..15
    quiet_between_taps N     0..15
    quiet_after_gesture N    0..15
    raw                      one-shot accelerometer reading (axis discovery)
    q                        quit
"""

import argparse
import asyncio
import sys
import time
from brilliant_ble import BrilliantBle

NUMERIC_KEYS = {
    "threshold",
    "max_peaks",
    "gesture_duration",
    "wait_for_timeout",
    "peak_duration",
    "shock_duration",
    "quiet_between_taps",
    "quiet_after_gesture",
}
STRING_KEYS = {"mode", "axis"}

t0 = time.monotonic()


def stamp() -> str:
    return f"{time.monotonic() - t0:8.3f}s"


def on_print(s: str) -> None:
    if s.startswith("TAP "):
        print(f"\n  [{stamp()}] >>> {s[4:].upper()} TAP <<<")
    else:
        print(f"\n  [{stamp()}] {s}")
    print("tune> ", end="", flush=True)


async def show_config(b: BrilliantBle) -> None:
    out = await b.send_lua(
        "do local c=frame.imu.tap_config() local s='' "
        "for k,v in pairs(c) do s=s..k..'='..tostring(v)..' ' end print(s) end",
        await_print=True,
    )
    print(f"  {out}")


async def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", default=None, help='exact BLE name, e.g. "Halo AB"')
    args = parser.parse_args()

    b = BrilliantBle()
    name = await b.connect(name=args.name, print_response_handler=on_print)
    await b.send_break_signal()

    fw = await b.send_lua("print(frame.FIRMWARE_VERSION)", await_print=True)
    print(f"Connected: {name} | firmware {fw}")

    await b.send_lua("frame.imu.tap_callback(function(k) print('TAP '..(k or '?')) end)")
    print("Tap callback armed (single/double/triple). Current config:")
    await show_config(b)
    print("Type 'help' for commands. Tap away.\n")

    loop = asyncio.get_event_loop()
    while True:
        line = await loop.run_in_executor(None, lambda: input("tune> "))
        line = line.strip()
        if not line:
            continue
        if line in ("q", "quit", "exit"):
            break
        if line == "help":
            print(__doc__)
            continue
        if line == "get":
            await show_config(b)
            continue
        if line == "raw":
            out = await b.send_lua(
                "do local a=frame.imu.raw().accelerometer "
                "print(string.format('ax %d ay %d az %d',a.x,a.y,a.z)) end",
                await_print=True,
            )
            print(f"  {out}")
            continue

        parts = line.split()
        if len(parts) != 2 or (parts[0] not in NUMERIC_KEYS and parts[0] not in STRING_KEYS):
            print("  ? unknown command (try 'help')")
            continue

        key, val = parts
        if key == "wait_for_timeout":
            lua_val = "true" if val not in ("0", "false") else "false"
        elif key in NUMERIC_KEYS:
            lua_val = val
        else:
            lua_val = f"'{val}'"

        out = await b.send_lua(
            f"print(pcall(frame.imu.tap_config, {{{key}={lua_val}}}))",
            await_print=True,
        )
        print(f"  -> {out}")

    await b.send_reset_signal()
    await b.disconnect()


try:
    asyncio.run(main())
except (KeyboardInterrupt, EOFError):
    sys.exit(0)
