"""
Shared BLE helpers for the Halo battery-modeling experiments.

Both live harnesses (discharge_log.py, charger_offset.py) import this. It wraps
brilliant_ble.BrilliantBle with:

  * device selection by exact name / substring / nearest,
  * a battery read that returns (level, voltage_mv, charging) in one REPL round-trip,
  * charge on/off + display-load control (display power-save toggles ~+20mA),
  * disconnect detection + bounded reconnect,

plus an append-only CSV+JSONL EventLog and the driver's voltage->SoC formula so the
host can reconstruct the raw (unfiltered) SoC the firmware would compute.

No firmware changes are required to run the experiments: the stock firmware already
exposes frame.battery_level()/battery_voltage()/battery_charging()/charge() and
frame.display.power_save().
"""

import asyncio
import csv
import json
import time
from datetime import datetime, timezone

from bleak import BleakScanner
from brilliant_ble import BrilliantBle, BrilliantDeviceType

# Brilliant custom GATT service that both Halo and Frame advertise.
SERVICE_UUID = "7a230001-5475-a6a4-654c-8431f6ad49c4"

# One REPL round-trip returns all three battery fields as "level,voltage,charging".
# tostring() is needed because Lua's ".." does not coerce booleans.
LUA_READ = (
    "print(frame.battery_level()..','..frame.battery_voltage()..','"
    "..tostring(frame.battery_charging()))"
)

# The firmware's voltage->SoC map lives in drivers/sensor/alif_vbat.c:256 and is a
# straight line: soc = (mV - 3200) / 9, clamped 0..100. Reproduce it here so we can
# compare the raw driver SoC against the EMA/hysteresis-filtered battery_level().
SOC_V0_MV = 3200
SOC_MV_PER_PCT = 9


def raw_soc_from_mv(voltage_mv: int) -> int:
    soc = (voltage_mv - SOC_V0_MV) // SOC_MV_PER_PCT
    return max(0, min(100, soc))


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


class LinkLost(Exception):
    """Raised when a REPL round-trip fails because the device went away."""


class NoDeviceFound(Exception):
    pass


async def scan(timeout: float = 5.0):
    """Return [(name, rssi)] of Brilliant devices in range, nearest first."""
    found = await BleakScanner.discover(timeout, return_adv=True)
    out = []
    for dev, adv in found.values():
        if SERVICE_UUID in adv.service_uuids:
            out.append((dev.name or "", adv.rssi))
    out.sort(key=lambda x: x[1], reverse=True)
    return out


async def resolve_name(selector, timeout: float = 5.0) -> str:
    """
    Resolve `selector` to an exact device local name (what BrilliantBle.connect wants).

      selector is None            -> nearest Brilliant device by RSSI
      selector matches a name     -> that exact name
      selector is any substring   -> first device whose name contains it (case-insensitive)
    """
    devices = await scan(timeout)
    if not devices:
        raise NoDeviceFound("no Brilliant devices advertising in range")
    if selector is None:
        return devices[0][0]
    sel = selector.lower()
    for name, _rssi in devices:
        if name.lower() == sel:
            return name
    for name, _rssi in devices:
        if sel in name.lower():
            return name
    seen = ", ".join(n or "?" for n, _ in devices)
    raise NoDeviceFound(f"no device matched {selector!r}; in range: {seen}")


class BatteryLink:
    """
    A resilient battery-reading connection to one Halo/Frame device.

    Selection is pinned to an exact name after the first connect so reconnects
    always target the same physical device (important when the dev kit and a real unit are
    both in range).
    """

    def __init__(self, selector=None, connect_timeout: float = 10.0):
        self._selector = selector
        self._name = None
        self._connect_timeout = connect_timeout
        self._ble = None
        self._down = False

    @property
    def name(self):
        return self._name

    @property
    def device_type(self):
        return self._ble.type if self._ble is not None else BrilliantDeviceType.UNKNOWN

    def _on_disconnect(self):
        self._down = True

    def is_connected(self) -> bool:
        return self._ble is not None and self._ble.is_connected() and not self._down

    async def connect(self, scan_timeout: float = 5.0):
        # Resolve by the original selector the first time; pin to the exact name after.
        selector = self._name or self._selector
        self._name = await resolve_name(selector, scan_timeout)
        self._ble = BrilliantBle()
        self._down = False
        await self._ble.connect(
            name=self._name,
            timeout=self._connect_timeout,
            print_response_handler=lambda _s: None,
            disconnect_handler=self._on_disconnect,
        )
        return self._name

    async def reconnect(self, attempts: int, delay: float, scan_timeout: float = 5.0) -> bool:
        """Try to re-establish the link. Returns True on success, False if all attempts fail."""
        for i in range(1, attempts + 1):
            try:
                await self.connect(scan_timeout=scan_timeout)
                return True
            except Exception as exc:  # NoDeviceFound, BleakError, timeout, ...
                print(f"  reconnect {i}/{attempts} failed: {exc}")
                if i < attempts:
                    await asyncio.sleep(delay)
        return False

    async def read_battery(self):
        """Return (level_pct:int, voltage_mv:int, charging:bool). Raises LinkLost on failure."""
        try:
            resp = await self._ble.send_lua(LUA_READ, await_print=True)
        except Exception as exc:
            self._down = True
            raise LinkLost(str(exc)) from exc
        try:
            # Lua prints numbers as floats ("57.0"), so coerce via float() first.
            level_s, volt_s, chg_s = resp.strip().split(",")
            return int(float(level_s)), int(float(volt_s)), chg_s.strip().lower() == "true"
        except (ValueError, AttributeError) as exc:
            raise LinkLost(f"unparseable battery response {resp!r}") from exc

    async def send(self, lua: str):
        """Fire-and-forget a Lua statement (no print expected). Raises LinkLost on failure."""
        try:
            await self._ble.send_lua(lua)
        except Exception as exc:
            self._down = True
            raise LinkLost(str(exc)) from exc

    async def set_charging(self, enable: bool):
        await self.send(f"frame.charge({'true' if enable else 'false'})")

    async def set_display_load(self, on: bool, color: int = 0x888888):
        """
        Hold a defined display load. The panel boots in power-save; taking it out of
        power-save adds ~20mA (=> ~30mA total), which is our 'display on' load. Base
        state (power-save on) is ~10mA. We draw a static fill first so the panel shows
        a defined, non-changing frame (no dynamic draw => no extra current variability).
        """
        if on:
            await self.send(f"frame.display.clear(0x{color:06X})")
            await self.send("frame.display.show(true)")
            await self.send("frame.display.power_save(false)")
        else:
            await self.send("frame.display.power_save(true)")

    async def disconnect(self):
        try:
            if self._ble is not None and self._ble.is_connected():
                await self._ble.disconnect()
        except Exception:
            pass


class EventLog:
    """Append-only CSV (with a JSONL mirror), flushed per row so an unattended run
    is never lost to a crash. Every record is stamped with wall time + monotonic t."""

    def __init__(self, path_stem: str, fieldnames):
        self.t0 = time.monotonic()
        self.csv_path = f"{path_stem}.csv"
        self.jsonl_path = f"{path_stem}.jsonl"
        self._fieldnames = ["wall_iso", "t_s"] + list(fieldnames)
        self._csv_f = open(self.csv_path, "w", newline="")
        self._csv = csv.DictWriter(self._csv_f, fieldnames=self._fieldnames)
        self._csv.writeheader()
        self._csv_f.flush()
        self._jsonl_f = open(self.jsonl_path, "w")

    def log(self, **fields):
        rec = {"wall_iso": now_iso(), "t_s": round(time.monotonic() - self.t0, 3)}
        rec.update(fields)
        row = {k: rec.get(k, "") for k in self._fieldnames}
        self._csv.writerow(row)
        self._csv_f.flush()
        self._jsonl_f.write(json.dumps(rec) + "\n")
        self._jsonl_f.flush()
        return rec

    def close(self):
        for f in (self._csv_f, self._jsonl_f):
            try:
                f.close()
            except Exception:
                pass
