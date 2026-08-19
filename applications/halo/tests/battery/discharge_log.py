#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10,<3.13"
# dependencies = [
#     "brilliant-ble",
# ]
# ///
# Note: pinned <3.13 for the same bleak/pyobjc reason as the aec harnesses.
"""
Unattended battery-discharge logger for the Halo battery model.

Holds a constant load (display on ~30mA, or off ~10mA) and polls
battery_level / battery_voltage / battery_charging over BLE every --interval
seconds, appending each sample to a CSV (+ JSONL mirror). Runs from a full
charge down to flat; when the battery dies the device stops advertising and,
after bounded reconnect attempts fail, the logger declares end-of-life,
writes a summary (runtime + estimated capacity), and exits.

This needs NO firmware change - it drives the stock Lua REPL. Use it on a real
unit for real numbers; on the dev kit it validates the harness
mechanics only (the dev kit runs off a bench PSU and never discharges).

  ./discharge_log.py --name-contains 08 --load on
  ./discharge_log.py --name "Halo AB" --load off --interval 120

Analyze the resulting CSV with analyze_battery.py.
"""

import argparse
import asyncio
import json
import signal
from datetime import datetime

from battery_common import BatteryLink, EventLog, LinkLost, raw_soc_from_mv

# Nominal average draw per load state, from the dev kit bench power supply.
LOAD_CURRENT_MA = {"on": 30.0, "off": 10.0}


def parse_args():
    p = argparse.ArgumentParser(description="Unattended Halo battery-discharge logger.")
    sel = p.add_mutually_exclusive_group()
    sel.add_argument("--name", help="exact device local name, e.g. 'Halo AB'")
    sel.add_argument("--name-contains", help="substring of the device name, e.g. '08'")
    p.add_argument("--load", choices=["on", "off"], default="on",
                   help="constant load to hold: display on (~30mA) or off (~10mA). Default on.")
    p.add_argument("--interval", type=float, default=60.0, help="seconds between samples (default 60)")
    p.add_argument("--capacity-mah", type=float, default=300.0, help="nominal battery capacity (default 300)")
    p.add_argument("--current-ma", type=float, default=None,
                   help="override assumed average draw (default: 30 for load on, 10 for off)")
    p.add_argument("--color", default="888888", help="display fill colour (hex RRGGBB) for load on")
    p.add_argument("--reconnect-attempts", type=int, default=6,
                   help="reconnect tries before declaring end-of-life (default 6)")
    p.add_argument("--reconnect-delay", type=float, default=30.0,
                   help="seconds between reconnect tries (default 30)")
    p.add_argument("--allow-charging", action="store_true",
                   help="do not abort if the device reports charging at start")
    p.add_argument("--out", help="output path stem (default discharge_<load>_<timestamp>)")
    return p.parse_args()


async def run(args):
    selector = args.name or args.name_contains
    current_ma = args.current_ma if args.current_ma is not None else LOAD_CURRENT_MA[args.load]
    stem = args.out or f"discharge_{args.load}_{datetime.now().strftime('%Y%m%d_%H%M%S')}"
    color = int(args.color, 16)

    log = EventLog(stem, ["event", "level_pct", "voltage_mv", "charging", "raw_soc"])
    link = BatteryLink(selector)

    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set)

    print(f"Scanning for {selector or 'nearest Brilliant device'} ...")
    name = await link.connect()
    print(f"Connected to {name} ({link.device_type.value}). Logging to {log.csv_path}")

    # Baseline read + charging guard.
    level, mv, charging = await link.read_battery()
    if charging and not args.allow_charging:
        print("ABORT: device reports charging - unplug it for a discharge run "
              "(or pass --allow-charging).")
        await link.disconnect()
        log.close()
        return 2

    print(f"Setting load: display {args.load} (~{current_ma:.0f} mA). "
          f"Start {level}% / {mv} mV.")
    await link.set_display_load(args.load == "on", color=color)

    log.log(event="start", level_pct=level, voltage_mv=mv, charging=charging,
            raw_soc=raw_soc_from_mv(mv))

    n = 0
    last_ok = None  # (t_s, level, mv)
    eol = False
    sched_start = loop.time()
    while not stop.is_set():
        # Fixed cadence: wake at sched_start + n*interval regardless of read latency.
        n += 1
        delay = max(0.0, (sched_start + n * args.interval) - loop.time())
        try:
            await asyncio.wait_for(stop.wait(), timeout=delay)
            break  # stop was set
        except asyncio.TimeoutError:
            pass

        try:
            level, mv, charging = await link.read_battery()
        except LinkLost as exc:
            print(f"link lost: {exc} -- attempting reconnect")
            log.log(event="link_lost", level_pct="", voltage_mv="", charging="", raw_soc="")
            ok = await link.reconnect(args.reconnect_attempts, args.reconnect_delay)
            if ok:
                print(f"reconnected to {link.name}; resuming")
                log.log(event="reconnected", level_pct="", voltage_mv="", charging="", raw_soc="")
                # Re-assert the load in case the device rebooted (fresh boot = power-save).
                try:
                    await link.set_display_load(args.load == "on", color=color)
                except LinkLost:
                    pass
                continue
            eol = True
            break

        rec = log.log(event="sample", level_pct=level, voltage_mv=mv, charging=charging,
                      raw_soc=raw_soc_from_mv(mv))
        last_ok = (rec["t_s"], level, mv)
        chg = " CHG" if charging else ""
        print(f"[{rec['t_s']:8.0f}s] {level:3d}%  {mv:4d} mV  raw={rec['raw_soc']:3d}%{chg}")

    # --- teardown / summary ---
    summary = {
        "device": link.name,
        "load": args.load,
        "assumed_current_ma": current_ma,
        "nominal_capacity_mah": args.capacity_mah,
        "samples": n,
        "end_of_life": eol,
    }
    if eol and last_ok is not None:
        t_flat_s, last_level, last_mv = last_ok
        runtime_h = t_flat_s / 3600.0
        est_mah = current_ma * runtime_h
        summary.update({
            "runtime_s": round(t_flat_s, 1),
            "runtime_h": round(runtime_h, 3),
            "last_level_pct": last_level,
            "last_voltage_mv": last_mv,
            "est_capacity_mah": round(est_mah, 1),
        })
        log.log(event="end_of_life", level_pct=last_level, voltage_mv=last_mv,
                charging=False, raw_soc=raw_soc_from_mv(last_mv))
        print(f"\nEND OF LIFE after {runtime_h:.2f} h (last {last_level}% / {last_mv} mV). "
              f"Estimated capacity ~{est_mah:.0f} mAh vs {args.capacity_mah:.0f} nominal.")
    else:
        print("\nStopped by user; battery not flat. Restoring display power-save.")
        try:
            await link.set_display_load(False)
        except LinkLost:
            pass
        await link.disconnect()

    with open(f"{stem}.summary.json", "w") as f:
        json.dump(summary, f, indent=2)
    log.close()
    print(f"Summary written to {stem}.summary.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(run(parse_args())))
