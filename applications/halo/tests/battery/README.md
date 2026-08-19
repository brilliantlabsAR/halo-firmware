# Battery-usage modeling

Tools to (1) measure and correct the **charger offset** (reported % is inflated while
charging and drops on unplug) and (2) measure true battery life and **linearize** the
reported % over a discharge.

## Why

Halo has no fuel-gauge IC. Battery % is a voltage-only estimate: `drivers/sensor/alif_vbat.c`
maps voltage→SoC with a straight line, `soc = (mV - 3200) / 9`, then `battery_manager.c`
applies EMA smoothing + charge-direction hysteresis. Two consequences:

- A LiPo's open-circuit voltage is very **non-linear** vs charge, so a linear map makes the
  reported % move non-linearly over time at constant draw.
- While charging, the terminal voltage sits **above** the resting level, so % reads high and
  visibly drops on unplug.

These experiments collect real data on a real unit and produce the lookup tables that replace
the linear map and cancel the charging offset. **No firmware change is needed to collect the
data** — the scripts drive the stock Lua REPL (`frame.battery_level/voltage/charging`,
`frame.charge`, `frame.display.power_save`).

## Setup

Scripts are self-contained [uv](https://docs.astral.sh/uv/) scripts — run them directly; uv
installs `brilliant-ble` (BLE) or `numpy`/`matplotlib` (analysis) on first run:

```
./discharge_log.py --help
```

Device selection (both live scripts): `--name "Halo AB"` (exact), `--name-contains AB`
(substring), or neither (nearest Brilliant device by RSSI). When both the dev kit and a real
unit are in range, always pin the one you mean.

## The three tools

| Script | Runs | Purpose |
|---|---|---|
| `discharge_log.py` | live BLE, unattended | Hold a constant load, poll battery every `--interval` s, log to CSV until flat. |
| `charger_offset.py` | live BLE | Relax-and-measure the charging offset across a charge cycle. |
| `analyze_battery.py` | offline | Turn the CSVs into battery-life numbers, linearity plots, and C lookup tables. |

`battery_common.py` is the shared BLE/logging helper.

### 1. Discharge run (battery life + linearity)

Constant-current discharge from 100% to flat. True SoC is then linear in elapsed time, so the
measured voltage-vs-time curve *is* the true voltage→SoC curve.

```
./discharge_log.py --name-contains 08 --load on          # ~30 mA, ~10 h
./discharge_log.py --name-contains 08 --load off --interval 120   # ~10 mA, ~30 h
```

- `--load on` takes the display out of power-save (~+20 mA ⇒ ~30 mA total) showing a static
  fill; `--load off` holds the ~10 mA base state. **Keep the speaker silent** — it dominates
  and destabilizes the draw.
- Aborts if the device reports charging at start (unplug it, or `--allow-charging`).
- Polls `battery_level`, `battery_voltage`, `battery_charging` each interval → `discharge_*.csv`
  (+ `.jsonl`). It also records the raw driver SoC `(mV-3200)/9` for comparison.
- When the battery dies the device stops advertising; after `--reconnect-attempts` fail it
  declares **end-of-life**, writes `*.summary.json` (runtime + estimated capacity), and exits.
  Transient dropouts are ridden out by the reconnect loop, so only a real flat ends the run.

Unattended: the load is a persistent device state, so it holds even if the host briefly drops.

### 2. Charger offset

Start with the battery **low and plugged in**, then:

```
./charger_offset.py --name-contains 08
```

Each cycle: read charging voltage → `frame.charge(false)` → wait `--relax` (120 s) → read
resting voltage → `frame.charge(true)` → log the pair + offset. Repeats every `--cadence`
(8 min) until full → `charger_offset_*.csv`.

**Safety:** charging is always re-enabled on exit (normal stop, Ctrl-C, or crash). If the link
is down at exit it warns you to reconnect and run `frame.charge(true)` — never leave a device
with charging disabled.

### 3. Analyze

```
./analyze_battery.py discharge discharge_on_YYYYmmdd_HHMMSS.csv --current-ma 30
./analyze_battery.py charger   charger_offset_YYYYmmdd_HHMMSS.csv
```

- discharge → battery life, estimated capacity, `voltage-vs-time` + linearity PNG, and a
  `static const struct discharge_point discharge_curve[]` C table for `alif_vbat.c`.
- charger → offset-vs-SoC PNG and a `charge_offset_mv[]` LUT to subtract while charging.
- `--current-ma` must match the discharge load (30 = display on, 10 = off); `--step` sets the
  emitted LUT granularity.

## Dev kit vs a real unit

Set up and validate all mechanics on the **dev kit** first: connect, load-set, polling,
CSV/JSONL, reconnect/end-of-life, charge toggle + safety re-enable. But the dev kit runs off a bench PSU
and **never discharges**, so:

- `discharge_log.py` on the dev kit: voltage stays pinned — validates the harness, not the curve.
  (Power the dev kit off manually to confirm end-of-life detection fires.)
- `charger_offset.py` on the dev kit: the offset reads ~0 — validates the protocol, not the numbers.

Real battery-life numbers and real curves require a **real Halo unit** discharged
from a full charge.

## Applying the results (firmware)

Phase 4 (`feat/battery-model`): replace the linear `voltage_to_soc()` in `alif_vbat.c` with
interpolation over the emitted `discharge_curve[]` (the `struct discharge_point` is already
declared there), and subtract the `charge_offset_mv[]` offset in `vbat_sample_fetch()` when
`data->is_charging`. Leave the `battery_manager.c` EMA/hysteresis layer as-is.
