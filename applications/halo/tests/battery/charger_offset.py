#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10,<3.13"
# dependencies = [
#     "brilliant-ble",
# ]
# ///
# Note: pinned <3.13 for the same bleak/pyobjc reason as the aec harnesses.
"""
Charger-offset characterisation for the Halo battery model.

While the device is on the charger, the battery voltage sits above its true
resting (open-circuit) level, so the reported % reads high and visibly drops on
unplug. This tool measures that offset as a function of charge level by, at each
sample point:

  1. read the charging voltage V_chg,
  2. disable charging (frame.charge(false)) and wait --relax seconds for the
     voltage to settle toward its resting level,
  3. read the resting voltage V_rest,
  4. re-enable charging (frame.charge(true)),
  5. log the pair + offset = soc(V_chg) - soc(V_rest).

Repeats across a full charge cycle (start it with the battery low and plugged in)
so the offset is swept over the whole range. The firmware correction then subtracts
this offset while charging so the reported % no longer jumps on unplug.

SAFETY: charging is always re-enabled on exit (normal stop, Ctrl-C, or crash) so
the device is never left with charging disabled.

  ./charger_offset.py --name-contains 08

Needs a real battery (production unit). On the dev kit (bench PSU) the mechanics run but
the offset will be ~0 - it validates the protocol, not the numbers.
"""

import argparse
import asyncio
import json
import signal
from datetime import datetime

from battery_common import BatteryLink, EventLog, LinkLost, raw_soc_from_mv


def parse_args():
    p = argparse.ArgumentParser(description="Halo charger-offset characterisation.")
    sel = p.add_mutually_exclusive_group()
    sel.add_argument("--name", help="exact device local name, e.g. 'Halo AB'")
    sel.add_argument("--name-contains", help="substring of the device name, e.g. '08'")
    p.add_argument("--relax", type=float, default=120.0,
                   help="seconds with charging disabled before the resting read (default 120)")
    p.add_argument("--cadence", type=float, default=480.0,
                   help="seconds of charging between offset samples (default 480 = 8 min)")
    p.add_argument("--presettle", type=float, default=15.0,
                   help="seconds of charging before the first charging read (default 15)")
    p.add_argument("--full-level", type=int, default=99,
                   help="resting level considered 'full' for the stop check (default 99)")
    p.add_argument("--full-hold", type=int, default=3,
                   help="consecutive full samples before stopping (default 3)")
    p.add_argument("--max-hours", type=float, default=6.0, help="hard stop after this long (default 6)")
    p.add_argument("--allow-not-charging", action="store_true",
                   help="do not abort if the device is not charging at start")
    p.add_argument("--out", help="output path stem (default charger_offset_<timestamp>)")
    return p.parse_args()


async def sleep_or_stop(stop: asyncio.Event, seconds: float) -> bool:
    """Sleep up to `seconds`; return True if stop was requested."""
    try:
        await asyncio.wait_for(stop.wait(), timeout=max(0.0, seconds))
        return True
    except asyncio.TimeoutError:
        return False


async def run(args):
    selector = args.name or args.name_contains
    stem = args.out or f"charger_offset_{datetime.now().strftime('%Y%m%d_%H%M%S')}"
    log = EventLog(stem, [
        "event", "level_chg", "v_chg", "raw_soc_chg",
        "level_rest", "v_rest", "raw_soc_rest", "offset_mv", "offset_soc",
    ])
    link = BatteryLink(selector)

    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set)

    print(f"Scanning for {selector or 'nearest Brilliant device'} ...")
    name = await link.connect()
    print(f"Connected to {name} ({link.device_type.value}). Logging to {log.csv_path}")

    level, mv, charging = await link.read_battery()
    print(f"Start {level}% / {mv} mV, charging={charging}.")
    if not charging and not args.allow_not_charging:
        print("ABORT: device is not charging - plug it in (or pass --allow-not-charging).")
        await link.disconnect()
        log.close()
        return 2

    samples = 0
    full_run = 0
    deadline = loop.time() + args.max_hours * 3600.0
    try:
        while not stop.is_set() and loop.time() < deadline:
            # Ensure charging is on and let the voltage re-elevate before the charging read.
            await link.set_charging(True)
            if await sleep_or_stop(stop, args.presettle if samples == 0 else 0.0):
                break

            level_chg, v_chg, chg_flag = await link.read_battery()

            # Relax: disable charging and wait for the voltage to settle toward OCV.
            await link.set_charging(False)
            if await sleep_or_stop(stop, args.relax):
                break
            level_rest, v_rest, rest_flag = await link.read_battery()

            # Resume charging immediately.
            await link.set_charging(True)

            offset_mv = v_chg - v_rest
            offset_soc = raw_soc_from_mv(v_chg) - raw_soc_from_mv(v_rest)
            samples += 1
            rec = log.log(
                event="offset",
                level_chg=level_chg, v_chg=v_chg, raw_soc_chg=raw_soc_from_mv(v_chg),
                level_rest=level_rest, v_rest=v_rest, raw_soc_rest=raw_soc_from_mv(v_rest),
                offset_mv=offset_mv, offset_soc=offset_soc,
            )
            print(f"[{rec['t_s']:7.0f}s] chg {level_chg:3d}% / {v_chg:4d} mV  ->  "
                  f"rest {level_rest:3d}% / {v_rest:4d} mV   "
                  f"offset {offset_mv:+4d} mV ({offset_soc:+3d}%)"
                  + ("" if chg_flag else "  [was not charging at chg-read!]"))

            # Stop once resting level has held 'full' for enough consecutive samples.
            full_run = full_run + 1 if level_rest >= args.full_level else 0
            if full_run >= args.full_hold:
                print(f"Battery full ({level_rest}% held x{full_run}). Stopping.")
                break

            # Charge for the remainder of the cadence before the next sample.
            if await sleep_or_stop(stop, max(0.0, args.cadence - args.relax - args.presettle)):
                break
    except LinkLost as exc:
        print(f"link lost: {exc}")
        log.log(event="link_lost")
    finally:
        # SAFETY: always try to leave charging enabled.
        try:
            if link.is_connected():
                await link.set_charging(True)
                print("Charging re-enabled.")
            else:
                print("WARNING: link down at exit - could not confirm charging re-enabled. "
                      "Reconnect and run frame.charge(true) if in doubt.")
        except LinkLost:
            print("WARNING: failed to re-enable charging on exit - check the device.")

    summary = {"device": link.name, "samples": samples,
               "relax_s": args.relax, "cadence_s": args.cadence}
    with open(f"{stem}.summary.json", "w") as f:
        json.dump(summary, f, indent=2)
    await link.disconnect()
    log.close()
    print(f"\n{samples} offset samples. Summary written to {stem}.summary.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(run(parse_args())))
