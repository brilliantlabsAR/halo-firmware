# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = ["brilliant-ble>=3.1.1,<4"]
# ///
"""
On-device regression test for the Lua RX GATT write handler
(modules/halo/src/ble_lua.c). Runs unattended on a dev kit (Halo AB).

It exercises the edge cases the write handler must survive and frame
correctly, writing raw bytes straight to the RX characteristic so it can
reach paths the SDK's own send_lua()/send_data() never produce:

  1. Baseline: a normal statement round-trips.
  2. A zero-length write is a harmless no-op and the device stays responsive.
  3. A burst of zero-length writes does not wedge or crash the device.
  4. A within-MTU statement executes as exactly one line (no lost byte, no
     spurious interior newline) - proven by evaluating an arithmetic
     expression whose printed result would change if a byte were dropped.
  5. An over-MTU single write (an ATT long/prepared write) is refused now
     that the characteristic sets NO_OFFSET; the device must survive and
     stay responsive whether the stack reports an error or drops it.

Usage:
    uv run test_ble_rx_write.py [--name "Halo AB"]

Exit code 0 = all checks passed.
"""

import argparse
import asyncio

from brilliant_ble import BrilliantBle

FAILURES = []


def check(cond, label):
    print(f"  {'PASS' if cond else 'FAIL'}: {label}")
    if not cond:
        FAILURES.append(label)


async def rx_raw_write(b, payload, response=True):
    """Write raw bytes to the device RX characteristic, bypassing the SDK's
    length guard. Returns None on success or the exception on failure."""
    try:
        await b._client.write_gatt_char(b._tx_characteristic, payload, response=response)
        return None
    except Exception as exc:  # noqa: BLE001 - we want to observe, not raise
        return exc


async def responsive(b):
    """True if the device still evaluates a REPL statement and prints back."""
    try:
        token = await b.send_lua("print('alive_'..tostring(2*21))", await_print=True, timeout=5)
        return token is not None and "alive_42" in token
    except Exception:
        return False


async def main():
    parser = argparse.ArgumentParser(
        description="Connect to a Halo/Frame dev kit over BLE and run the RX write regression test."
    )
    parser.add_argument(
        "--name",
        default=None,
        help='exact BLE device name, e.g. "Halo AB"; defaults to the nearest device',
    )
    args = parser.parse_args()

    b = BrilliantBle()
    name = await b.connect(name=args.name, print_response_handler=lambda s: None)
    # The device boots into main.lua, which prints to the same channel as REPL
    # responses. Flush any stray app output so the first await_print below
    # reads a REPL reply and not an app line.
    await b.drain_print_channel()
    fw = await b.send_lua("print(frame.FIRMWARE_VERSION)", await_print=True)
    mtu = b._client.mtu_size
    print(f"{name} | firmware {fw} | MTU {mtu}")

    # 1. Baseline round-trip.
    check(await responsive(b), "baseline statement round-trips")

    # 2. Single zero-length write is a no-op; device stays responsive.
    err = await rx_raw_write(b, b"")
    check(await responsive(b), "device responsive after a zero-length write")

    # 3. Burst of zero-length writes does not wedge the device.
    for _ in range(20):
        await rx_raw_write(b, b"", response=False)
    await asyncio.sleep(0.2)
    check(await responsive(b), "device responsive after 20 zero-length writes")

    # 4. A within-MTU statement executes as exactly one line. The expression
    #    is chosen so a dropped byte or an injected newline changes the result
    #    (or makes it error out and print nothing).
    stmt = "print('sum='..tostring(111+222+333))"
    assert len(stmt) <= mtu - 3, "statement must fit one packet for this check"
    got = await b.send_lua(stmt, await_print=True, timeout=5)
    check(got is not None and "sum=666" in got,
          f"within-MTU statement is one clean line (got {got!r})")

    # 5. Over-MTU single write is refused (NO_OFFSET). Build a payload larger
    #    than one packet so the stack would have to long-write it.
    big = b"print('X" + b"y" * (mtu + 40) + b"')"
    err = await rx_raw_write(b, big, response=True)
    outcome = "rejected by stack" if err is not None else "accepted/dropped by controller"
    print(f"    over-MTU write: {outcome}")
    check(await responsive(b), "device responsive after an over-MTU write")

    await b.disconnect()

    print()
    if FAILURES:
        print(f"FAILED ({len(FAILURES)}): " + "; ".join(FAILURES))
        raise SystemExit(1)
    print("OK")


asyncio.run(main())
