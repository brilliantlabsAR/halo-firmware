# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = ["brilliant-ble>=3.1.1,<4"]
# ///
"""
Post-flash smoke test: connect to a named Halo device and check the Lua REPL
answers. Prints the running firmware version. Exits non-zero on failure.

The device boots into whatever main.lua is present; if that has a main loop,
REPL commands time out. So this sends a break signal right after connecting,
probes the REPL, then resets the Lua VM so main.lua runs again.

Usage:
    uv run verify.py --name "Halo AB"
"""
import argparse
import asyncio
import sys

from brilliant_ble import BrilliantBle


async def main():
    parser = argparse.ArgumentParser(description="Verify a Halo device boots and its Lua REPL responds")
    parser.add_argument("--name", required=True, help='exact BLE name, e.g. "Halo AB"')
    args = parser.parse_args()

    halo = BrilliantBle()
    try:
        name = await halo.connect(name=args.name)
        print(f"Connected to {name}")
        # break any running main.lua out of its loop so the REPL answers
        await halo.send_break_signal()
        halo._user_print_response_handler = print
        await halo.send_lua("print('fw ' .. frame.FIRMWARE_VERSION)", await_print=True)
        await halo.send_lua("print('repl-ok')", await_print=True)
        # restart the Lua VM so the device goes back to running main.lua
        await halo.send_reset_signal()
    except Exception as e:
        print(f"Verification failed: {e}")
        sys.exit(1)
    finally:
        await halo.disconnect()


if __name__ == "__main__":
    asyncio.run(main())
