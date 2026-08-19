# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = ["brilliant-ble>=3.1.1,<4"]
# ///
"""
Fetch persisted /lfs log files from a Halo device via the frame.log Lua API,
then restart main.lua so the device returns to its normal app.

Self-contained: run with `uv run fetch_logs.py --name "Halo AB"` from anywhere.
Sends a break signal after connecting (so a running main.lua doesn't swallow
the REPL), streams each log file out in chunks, then resets the Lua VM.
"""
import argparse
import asyncio
import sys

from brilliant_ble import BrilliantBle

CHUNK = 180


async def main():
    parser = argparse.ArgumentParser(description="Fetch persisted /lfs logs from a Halo device over BLE")
    parser.add_argument("--name", required=True, help='exact BLE name, e.g. "Halo AB"')
    parser.add_argument("--chunk", type=int, default=CHUNK, help=f"Lua read chunk size (default {CHUNK})")
    args = parser.parse_args()

    halo = BrilliantBle()
    try:
        await halo.connect(name=args.name)
        print(f"# connected to {args.name}", flush=True)

        await halo.send_break_signal()
        await asyncio.sleep(0.3)

        files = await halo.send_lua(
            'local t={} for _,f in ipairs(frame.log.list()) do '
            't[#t+1]=f.name..":"..f.size end print(table.concat(t,","))',
            await_print=True, timeout=10)
        print(f"# log files: {files}", flush=True)

        for entry in [e.strip() for e in files.split(",") if e.strip()]:
            name = entry.split()[0].split(":")[0]
            print(f"\n===== {entry} =====", flush=True)
            # Load file into a global, then stream it out in chunks.
            await halo.send_lua(f'__l = frame.log.read("{name}") print(#__l)',
                                await_print=True, timeout=15)
            size = await halo.send_lua('print(#__l)', await_print=True, timeout=10)
            size = int(size)
            i = 1
            while i <= size:
                chunk = await halo.send_lua(
                    f'print(string.sub(__l,{i},{i + args.chunk - 1}))',
                    await_print=True, timeout=10)
                print(chunk, end="", flush=True)
                i += args.chunk
            await halo.send_lua('__l = nil print(1)', await_print=True, timeout=10)

        print("\n# restarting main.lua", flush=True)
        await halo.send_reset_signal()
    except Exception as e:
        print(f"\nLog fetch failed: {e}")
        sys.exit(1)
    finally:
        await halo.disconnect()


if __name__ == "__main__":
    asyncio.run(main())
