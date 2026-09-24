# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = ["bleak>=0.22"]
# ///
"""
Flashes signed app firmware to a Halo sitting in the BOOTLOADER's BLE DFU
recovery mode (MCUboot + MCUmgr over SMP).

Use this when ota_flash.py cannot be used, i.e. the device is in DFU recovery
mode rather than running the app. That happens when:
  * MCUboot found no bootable image and entered DFU mode by itself, or
  * the button was held ~10 s through a power-cycle (note: button entry also
    FORMATS /lfs, erasing main.lua, user files and BLE bonds).

Why a separate script: ota_flash.py goes through brilliant_ble, whose connect()
requires the app's Frame GATT service to bind its Lua characteristics. The
bootloader exposes only the SMP service, so that call fails before OTA starts.
This talks SMP directly and depends on bleak alone, so it keeps working even if
brilliant-ble moves on -- it is a recovery tool and should have no avoidable
dependencies.

It also works against a device running the app (the app exposes SMP too), so
when in doubt this script is the safe choice.

Usage:
    uv run dfu_flash.py --name "Halo AB"                        # report slot state only
    uv run dfu_flash.py zephyr.signed.bin --name "Halo AB"      # one-shot test boot (default)
    uv run dfu_flash.py zephyr.signed.bin --name "Halo AB" --yes

Flags mirror ota_flash.py. As there, the default marks the image for a one-shot
test boot: the app self-confirms after a clean boot, otherwise MCUboot reverts.
If a recovery image boots but hangs before self-confirming, the device simply
returns to DFU mode -- which is the desired outcome, not a worse brick.

After a button-entry DFU session the device has no bonds while the host may
still hold one, which shows up as "Peer removed pairing information" or
"Encryption is insufficient". Forget the device on the host and retry.
"""
import argparse
import asyncio
import hashlib
import sys

from bleak import BleakClient, BleakScanner

SMP_SERVICE_UUID = "8d53dc1d-1db7-4cd3-868b-8a527460aa84"
SMP_CHAR_UUID = "da2e7828-fbce-4e01-ae9e-261174997c48"
APP_SERVICE_PREFIX = "7a230001"

OP_READ, OP_READ_RSP, OP_WRITE, OP_WRITE_RSP = 0, 1, 2, 3
GROUP_OS, GROUP_IMAGE = 0, 1
ID_IMAGE_STATE, ID_IMAGE_UPLOAD, ID_OS_RESET = 0, 1, 5


class DfuError(Exception):
    pass


# --- minimal CBOR, definite-length, covering what MCUmgr uses ----------------

def _head(major: int, value: int) -> bytes:
    if value < 24:
        return bytes([(major << 5) | value])
    for bits, extra in ((8, 24), (16, 25), (32, 26), (64, 27)):
        if value < (1 << bits):
            return bytes([(major << 5) | extra]) + value.to_bytes(bits // 8, "big")
    raise DfuError("value too large to encode")


def cbor_encode(value) -> bytes:
    if isinstance(value, bool):
        return bytes([0xF5 if value else 0xF4])
    if value is None:
        return bytes([0xF6])
    if isinstance(value, int):
        return _head(0, value) if value >= 0 else _head(1, -value - 1)
    if isinstance(value, (bytes, bytearray)):
        return _head(2, len(value)) + bytes(value)
    if isinstance(value, str):
        raw = value.encode()
        return _head(3, len(raw)) + raw
    if isinstance(value, (list, tuple)):
        return _head(4, len(value)) + b"".join(cbor_encode(v) for v in value)
    if isinstance(value, dict):
        # None-valued keys are omitted, matching how MCUmgr peers expect
        # optional fields to be absent rather than explicitly null.
        entries = [(k, v) for k, v in value.items() if v is not None]
        out = _head(5, len(entries))
        for k, v in entries:
            out += cbor_encode(k) + cbor_encode(v)
        return out
    raise DfuError(f"cannot encode {type(value).__name__}")


_BREAK = object()


def cbor_decode(data: bytes):
    pos = 0

    def take(n: int) -> bytes:
        nonlocal pos
        if pos + n > len(data):
            raise DfuError("truncated CBOR response")
        chunk = data[pos:pos + n]
        pos += n
        return chunk

    def length(additional: int):
        if additional < 24:
            return additional
        if additional in (24, 25, 26, 27):
            return int.from_bytes(take(1 << (additional - 24)), "big")
        if additional == 31:
            return None  # indefinite
        raise DfuError(f"bad CBOR additional info {additional}")

    def item():
        nonlocal pos
        initial = take(1)[0]
        major, additional = initial >> 5, initial & 0x1F
        if major == 7:
            if additional == 20:
                return False
            if additional == 21:
                return True
            if additional in (22, 23):
                return None
            if additional == 31:
                return _BREAK
            raise DfuError(f"unsupported CBOR simple value {additional}")
        n = length(additional)
        if major == 0:
            return n
        if major == 1:
            return -1 - n
        if major in (2, 3):
            if n is None:
                parts = []
                while True:
                    part = item()
                    if part is _BREAK:
                        break
                    parts.append(part)
                joined = b"".join(parts) if major == 2 else "".join(parts)
                return joined
            raw = take(n)
            return raw if major == 2 else raw.decode(errors="replace")
        if major == 4:
            out = []
            if n is None:
                while True:
                    v = item()
                    if v is _BREAK:
                        break
                    out.append(v)
            else:
                out = [item() for _ in range(n)]
            return out
        if major == 5:
            out = {}
            if n is None:
                while True:
                    k = item()
                    if k is _BREAK:
                        break
                    out[k] = item()
            else:
                for _ in range(n):
                    k = item()
                    out[k] = item()
            return out
        raise DfuError(f"unsupported CBOR major type {major}")

    if not data:
        return {}
    return item()


# --- SMP transport ----------------------------------------------------------

class SmpClient:
    """Sends SMP requests over one GATT characteristic, matching by sequence."""

    def __init__(self, write):
        self._write = write
        self._sequence = 0
        self._rx = b""
        self._pending = {}

    def feed(self, chunk: bytes):
        self._rx += bytes(chunk)
        while len(self._rx) >= 8:
            total = 8 + int.from_bytes(self._rx[2:4], "big")
            if len(self._rx) < total:
                return
            packet, self._rx = self._rx[:total], self._rx[total:]
            waiter = self._pending.pop(packet[6], None)
            if waiter is None:
                continue
            future, expected_op = waiter
            if future.done():
                continue
            try:
                payload = cbor_decode(packet[8:])
            except DfuError as e:
                future.set_exception(e)
                continue
            if packet[0] != expected_op:
                future.set_exception(DfuError(f"unexpected SMP op {packet[0]}"))
            elif isinstance(payload, dict) and payload.get("rc"):
                future.set_exception(DfuError(f"SMP error rc={payload['rc']}"))
            else:
                future.set_result(payload)

    async def request(self, op, group, id, payload, timeout=8.0):
        body = cbor_encode(payload)
        seq = self._sequence
        self._sequence = (self._sequence + 1) & 0xFF
        packet = (bytes([op, 0]) + len(body).to_bytes(2, "big")
                  + group.to_bytes(2, "big") + bytes([seq, id]) + body)
        future = asyncio.get_running_loop().create_future()
        self._pending[seq] = (future, OP_READ_RSP if op == OP_READ else OP_WRITE_RSP)
        try:
            await self._write(packet)
            return await asyncio.wait_for(future, timeout)
        except asyncio.TimeoutError:
            raise DfuError(f"SMP response timed out (group {group}, id {id})")
        finally:
            self._pending.pop(seq, None)


def describe(images):
    for img in images or []:
        if not isinstance(img, dict):
            continue
        h = img.get("hash")
        h = h.hex() if isinstance(h, (bytes, bytearray)) else h
        flags = ",".join(k for k in ("bootable", "pending", "confirmed", "active", "permanent")
                         if img.get(k))
        print(f"  slot {img.get('slot')}: v{img.get('version')} {h} [{flags}]")


async def main():
    parser = argparse.ArgumentParser(
        description="Flash a Halo in bootloader BLE DFU recovery mode over SMP")
    parser.add_argument("firmware", nargs="?",
                        help="path to zephyr.signed.bin (omit to only report slot state)")
    parser.add_argument("--name", required=True,
                        help='exact BLE name, e.g. "Halo AB"')
    parser.add_argument("--yes", action="store_true",
                        help="skip the y/N confirmation prompt")
    parser.add_argument("--dangerously-auto-confirm", action="store_true",
                        help="confirm the image immediately instead of marking it for a "
                             "one-shot test boot")
    parser.add_argument("--chunk-size", type=int, default=384,
                        help="upload payload bytes per packet (default 384)")
    parser.add_argument("--retries", type=int, default=4,
                        help="connection attempts; drops with reason 0x98 are common "
                             "(default 4)")
    args = parser.parse_args()

    # Retries cover getting connected. Once bytes are going out we must not
    # silently start a second ~600 KB upload from scratch, so a failure after
    # that point is reported instead.
    progress = {"uploading": False}
    for attempt in range(1, args.retries + 1):
        try:
            return await run(args, progress)
        except Exception as e:
            # Some bleak errors stringify to "", so always name the type --
            # an unexplained failure is useless when recovering a device.
            detail = f"{type(e).__name__}: {e}" if str(e) else type(e).__name__
            if progress["uploading"]:
                print(f"\nFailed after the upload started -- {detail}")
                print("Not retrying automatically; re-run the command to upload again.")
                return 1
            if attempt == args.retries:
                print(f"Failed after {attempt} attempt(s) -- {detail}")
                return 1
            print(f"attempt {attempt} failed ({detail}); retrying")
            await asyncio.sleep(3)


async def run(args, progress):
    device = await BleakScanner.find_device_by_name(args.name, timeout=15.0)
    if device is None:
        raise DfuError(f"no device advertising as {args.name!r}")

    async with BleakClient(device, timeout=30.0) as client:
        uuids = [s.uuid.lower() for s in client.services]
        if not any(SMP_SERVICE_UUID in u for u in uuids):
            raise DfuError("no SMP service; this firmware does not support OTA")
        in_app = any(APP_SERVICE_PREFIX in u for u in uuids)
        print(f"Connected to {args.name} "
              f"({'app is running' if in_app else 'BOOTLOADER DFU MODE'})")

        smp = SmpClient(
            lambda pkt: client.write_gatt_char(SMP_CHAR_UUID, pkt, response=False))
        await client.start_notify(SMP_CHAR_UUID, lambda _, data: smp.feed(data))

        state = await smp.request(OP_READ, GROUP_IMAGE, ID_IMAGE_STATE, {}, timeout=20.0)
        print("Image state:")
        describe(state.get("images"))

        if args.firmware is None:
            return 0

        with open(args.firmware, "rb") as f:
            firmware = f.read()

        if not args.yes:
            answer = input(f"Flash {args.firmware} to {args.name!r}? [y/N] ").strip().lower()
            if answer != "y":
                print("Aborted. No firmware was written.")
                return 0

        sha = hashlib.sha256(firmware).digest()
        total = len(firmware)
        progress["uploading"] = True
        off = 0
        while off < total:
            chunk = firmware[off:off + args.chunk_size]
            payload = ({"image": 0, "len": total, "sha": sha, "off": off, "data": chunk}
                       if off == 0 else {"off": off, "data": chunk})
            rsp = await smp.request(OP_WRITE, GROUP_IMAGE, ID_IMAGE_UPLOAD, payload,
                                    timeout=90.0 if off == 0 else 30.0)
            off = rsp["off"] if isinstance(rsp.get("off"), int) else off + len(chunk)
            print(f"\rUploaded {off}/{total} bytes ({off * 100 // total}%)",
                  end="", flush=True)
        print()

        state = await smp.request(OP_READ, GROUP_IMAGE, ID_IMAGE_STATE, {}, timeout=20.0)
        images = [i for i in (state.get("images") or [])
                  if isinstance(i, dict) and isinstance(i.get("hash"), (bytes, bytearray))]
        candidate = (next((i for i in images if i.get("slot") == 1), None)
                     or next((i for i in images if i.get("active") is False), None))
        if candidate is None:
            raise DfuError("no uploaded image found in slot 1")
        image_hash = candidate["hash"]
        print(f"Flashed image with MCUboot hash {image_hash.hex()}")

        # Marking an image pending when it is byte-identical to the running,
        # confirmed image is refused with rc=1: there is nothing to swap to.
        # Observed in both app and DFU mode, so it is about the image rather
        # than which server answers. Detect it rather than issue a request we
        # know will fail.
        active = next((i for i in images if i.get("active")), None)
        if active is not None and active.get("hash") == image_hash:
            print("Uploaded image is identical to the image already running and "
                  "confirmed; nothing to mark and no reboot needed")
            return 0

        await smp.request(OP_WRITE, GROUP_IMAGE, ID_IMAGE_STATE,
                          {"hash": image_hash, "confirm": args.dangerously_auto_confirm})
        print("Image confirmed" if args.dangerously_auto_confirm else
              "Image marked for test boot: it sticks once the app self-confirms after a "
              "clean boot, otherwise MCUboot reverts")

        try:
            await smp.request(OP_WRITE, GROUP_OS, ID_OS_RESET, {}, timeout=3.0)
        except Exception:
            pass  # the device disconnects as it reboots
        print("Device is rebooting...")
        return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()) or 0)
