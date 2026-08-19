#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = [
#     "numpy",
#     "matplotlib",
# ]
# ///
"""
Offline analysis for the Halo battery-model experiments. No BLE.

  ./analyze_battery.py discharge discharge_on_YYYYmmdd_HHMMSS.csv
  ./analyze_battery.py charger   charger_offset_YYYYmmdd_HHMMSS.csv

DISCHARGE: assumes a constant-current run, so true state-of-charge is linear in
elapsed time: true_soc(t) = 100 * (1 - t / t_flat). It reports battery life and
estimated capacity, plots voltage-vs-time and reported-%/raw-%/true-% linearity,
and emits a `discharge_point discharge_curve[]` C table (voltage -> soc) to paste
into drivers/sensor/alif_vbat.c in place of the linear (mV-3200)/9 map.

CHARGER: from the relax-and-measure pairs, tabulates the charging offset vs resting
SoC and emits an offset LUT for the firmware to subtract while charging.

The firmware's current linear map is soc = (mV - 3200) / 9 (alif_vbat.c:256); the
raw_soc column in the CSVs is that map applied to the logged voltage.
"""

import argparse
import csv
import sys

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def read_rows(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def _f(row, key):
    v = row.get(key, "")
    return float(v) if v not in ("", None) else np.nan


# ---------------------------------------------------------------- discharge ----

def analyze_discharge(path, args):
    rows = [r for r in read_rows(path) if r.get("event") in ("sample", "start")]
    if len(rows) < 5:
        sys.exit("discharge: need at least a few 'sample' rows to analyze")

    t = np.array([_f(r, "t_s") for r in rows])
    v = np.array([_f(r, "voltage_mv") for r in rows])
    level = np.array([_f(r, "level_pct") for r in rows])
    raw = np.array([_f(r, "raw_soc") for r in rows])

    t_flat = args.t_flat if args.t_flat else float(t[-1])
    runtime_h = t_flat / 3600.0
    est_mah = args.current_ma * runtime_h
    true_soc = np.clip(100.0 * (1.0 - t / t_flat), 0.0, 100.0)

    print(f"== discharge: {path} ==")
    print(f"samples            : {len(rows)}")
    print(f"battery life        : {runtime_h:.2f} h  (t_flat = {t_flat:.0f} s)")
    print(f"assumed avg current : {args.current_ma:.1f} mA")
    print(f"estimated capacity  : {est_mah:.0f} mAh  (nominal {args.capacity_mah:.0f})")
    print(f"voltage span        : {np.nanmax(v):.0f} -> {np.nanmin(v):.0f} mV")

    # Linearity of the current reported % against true (coulomb-counted) %.
    dev_level = np.nanmax(np.abs(level - true_soc))
    dev_raw = np.nanmax(np.abs(raw - true_soc))
    print(f"max |reported - true|: {dev_level:.1f} %   (this is the non-linearity to fix)")
    print(f"max |raw_soc  - true|: {dev_raw:.1f} %")

    # Build voltage(true_soc) then sample it on a SoC grid -> (voltage, soc) LUT.
    order = np.argsort(true_soc)  # ascending SoC
    soc_sorted = true_soc[order]
    v_sorted = v[order]
    soc_grid = np.arange(0, 100 + args.step, args.step)
    v_grid = np.interp(soc_grid, soc_sorted, v_sorted)
    v_grid = np.maximum.accumulate(np.round(v_grid).astype(int))  # enforce monotonic-in-SoC

    print("\n// Fitted discharge curve for drivers/sensor/alif_vbat.c")
    print("// (replaces the linear voltage_to_soc map; interpolate between points)")
    print("static const struct discharge_point discharge_curve[] = {")
    for mv, soc in zip(v_grid, soc_grid):
        print(f"\t{{ {int(mv):4d}, {int(soc):3d} }},")
    print("};")

    if not args.no_plot:
        stem = path.rsplit(".", 1)[0]
        fig, ax = plt.subplots(1, 2, figsize=(12, 4.5))
        ax[0].plot(t / 3600.0, v, ".-", ms=3)
        ax[0].set_xlabel("time (h)"); ax[0].set_ylabel("battery mV"); ax[0].set_title("Voltage vs time")
        ax[0].grid(True, alpha=0.3)
        ax[1].plot(true_soc, level, ".", ms=4, label="reported (filtered)")
        ax[1].plot(true_soc, raw, ".", ms=3, alpha=0.5, label="raw (mV-3200)/9")
        ax[1].plot([0, 100], [0, 100], "k--", lw=1, label="ideal (linear)")
        ax[1].set_xlabel("true SoC (coulomb) %"); ax[1].set_ylabel("reported %")
        ax[1].set_title("Linearity"); ax[1].legend(); ax[1].grid(True, alpha=0.3)
        fig.tight_layout()
        fig.savefig(f"{stem}.png", dpi=110)
        print(f"\nplot -> {stem}.png")


# ------------------------------------------------------------------ charger ----

def analyze_charger(path, args):
    rows = [r for r in read_rows(path) if r.get("event") == "offset"]
    if len(rows) < 2:
        sys.exit("charger: need at least a couple of 'offset' rows to analyze")

    rest_soc = np.array([_f(r, "raw_soc_rest") for r in rows])
    off_mv = np.array([_f(r, "offset_mv") for r in rows])
    off_soc = np.array([_f(r, "offset_soc") for r in rows])

    print(f"== charger offset: {path} ==")
    print(f"samples             : {len(rows)}")
    print(f"resting SoC span     : {np.nanmin(rest_soc):.0f} -> {np.nanmax(rest_soc):.0f} %")
    print(f"offset span          : {np.nanmin(off_mv):.0f} .. {np.nanmax(off_mv):.0f} mV "
          f"({np.nanmin(off_soc):.0f} .. {np.nanmax(off_soc):.0f} %)")

    # Bucket the offset by resting SoC and average -> LUT.
    order = np.argsort(rest_soc)
    rs, om = rest_soc[order], off_mv[order]
    soc_grid = np.arange(0, 100 + args.step, args.step)
    off_grid = np.round(np.interp(soc_grid, rs, om)).astype(int)

    print("\n// Charging offset LUT: subtract offset_mv(soc) from the measured voltage")
    print("// while charging, before the discharge-curve lookup (alif_vbat.c vbat_sample_fetch).")
    print("static const struct discharge_point charge_offset_mv[] = {  /* {soc, offset_mv} */")
    for soc, mv in zip(soc_grid, off_grid):
        print(f"\t{{ {int(soc):3d}, {int(mv):4d} }},")
    print("};")

    if not args.no_plot:
        stem = path.rsplit(".", 1)[0]
        fig, ax = plt.subplots(figsize=(7, 4.5))
        ax.plot(rest_soc, off_mv, "o", ms=5, label="measured")
        ax.plot(soc_grid, off_grid, "-", label="LUT")
        ax.set_xlabel("resting SoC %"); ax.set_ylabel("charging offset (mV)")
        ax.set_title("Charger voltage offset vs charge level"); ax.grid(True, alpha=0.3); ax.legend()
        fig.tight_layout()
        fig.savefig(f"{stem}.png", dpi=110)
        print(f"\nplot -> {stem}.png")


def main():
    p = argparse.ArgumentParser(description="Analyze Halo battery experiment CSVs.")
    p.add_argument("kind", choices=["discharge", "charger"])
    p.add_argument("csv")
    p.add_argument("--current-ma", type=float, default=30.0,
                   help="assumed avg draw for the discharge run (default 30 = display on)")
    p.add_argument("--capacity-mah", type=float, default=300.0)
    p.add_argument("--t-flat", type=float, default=None,
                   help="override end-of-life time (s); default = last sample's t_s")
    p.add_argument("--step", type=int, default=5, help="SoC grid step for emitted LUTs (default 5)")
    p.add_argument("--no-plot", action="store_true")
    args = p.parse_args()

    if args.kind == "discharge":
        analyze_discharge(args.csv, args)
    else:
        analyze_charger(args.csv, args)


if __name__ == "__main__":
    main()
