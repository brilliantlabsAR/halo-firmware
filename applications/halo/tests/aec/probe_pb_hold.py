#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10,<3.13"
# dependencies = [
#     "brilliant-msg",
# ]
# ///
"""Behavioral flash-verify probe: confirm the new sup_pb_hold telemetry field
is present in the live aec('stats') dump. VERSION/GIT_TAG do not prove a build
landed, so key on a field that only exists in the reference-collapse-hold build.

Run: PYTHONUNBUFFERED=1 uv run probe_pb_hold.py --name "Halo AB"
"""
import argparse
import asyncio

from brilliant_ble import BrilliantDeviceType
from brilliant_msg import BrilliantMsg


async def main():
    p = argparse.ArgumentParser()
    p.add_argument("--name", required=True)
    args = p.parse_args()

    frame = BrilliantMsg()
    printed = []
    try:
        await frame.ble.connect(
            name=args.name,
            data_response_handler=frame._handle_data_response)
        await frame.ble.send_break_signal()
        await frame.ble.send_reset_signal()
        await frame.ble.send_break_signal()
        if frame.ble.type != BrilliantDeviceType.HALO:
            print("This probe requires a Halo")
            return

        frame.attach_print_response_handler(lambda s: printed.append(s))
        # dump the stats table in short batches (a single print overflows)
        await frame.ble.send_lua(
            "local s=frame.microphone.diag('stats') local o='' "
            "for k,v in pairs(s) do o=o..k..'='..tostring(v)..' ' "
            "if #o>150 then print(o) o='' end end "
            "if #o>0 then print(o) end print('#end')",
            await_print=True, timeout=10)
        for _ in range(100):
            if any(pp.strip().endswith("#end") for pp in printed):
                break
            await asyncio.sleep(0.05)

        stats = {}
        for line in printed:
            for kv in line.split():
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    stats[k] = v

        present = "sup_pb_hold" in stats
        print(f"\n=== PROBE RESULT ===")
        print(f"sup_pb_hold present: {present}"
              + (f" (value={stats['sup_pb_hold']})" if present else ""))
        # show a couple of neighbouring gate fields for sanity
        for k in ("sup_gate_rel", "sup_gmean", "p_ref"):
            if k in stats:
                print(f"  {k}={stats[k]}")
        print("PROBE_OK" if present else "PROBE_FAIL: field missing "
              "-> old firmware still resident")
    finally:
        try:
            await frame.ble.send_break_signal()
            await frame.ble.send_reset_signal()
        except Exception:
            pass
        try:
            await frame.ble.disconnect()
        except Exception:
            pass


if __name__ == "__main__":
    asyncio.run(main())
