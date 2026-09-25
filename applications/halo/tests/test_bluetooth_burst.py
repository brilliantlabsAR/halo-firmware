# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = ["brilliant-ble>=3.3.0,<4"]
# ///
"""
Regression test for frame.bluetooth.receive_callback delivery under load.

Every framed data write (0x01 marker) must reach the Lua callback exactly
once, as one string of exactly the bytes written, in arrival order - no
matter what the script is doing when the packets land. Packets that arrive
while the script is blocked or busy queue in the ble_lua frame ring and are
drained by the runtime's shared event hook (modules/halo/src/lua_runtime.c).

Scenarios:
  1. sequential write-with-response while idle (control)
  2. write-without-response burst while idle
  3. burst while the script is blocked in frame.sleep()
  4. burst while the script spins in a Lua loop
  5. maximum-size frames (one full ATT write each)
  6. ring back-pressure: more bytes than the RX queue holds while the script
     sleeps - writes must be refused (ATT error), never silently dropped, and
     every accepted write must still be delivered
  7. Ctrl+C while a burst is queued (data hook must not swallow the break)
  8. Ctrl+D (VM restart) while the script is busy
  9. packets with no callback registered are discarded, and a callback
     registered afterwards receives new packets

Each packet is [seq byte] + filler, so the device-side log records
"<len>/<seq>" per callback and the host checks count, order and sizes.

Usage:
    uv run test_bluetooth_burst.py [--name "Halo AB"]

Exit code 0 = all checks passed.
"""

import argparse
import asyncio

from brilliant_ble import BrilliantBle
from halo_device_file import safe_teardown

FAILURES = []


def check(cond, label):
    print(f"  {'PASS' if cond else 'FAIL'}: {label}")
    if not cond:
        FAILURES.append(label)


# Lua side: count callbacks and record "<len>/<seq>" for each.
LUA_SETUP = "N=0;S={};function cb(d) N=N+1; S[#S+1]=#d..'/'..d:byte(1) end"
LUA_RESET = "N=0;S={}"
LUA_REPORT = "print(N..' '..table.concat(S,' '))"


def pkt(seq, size):
    return bytes([seq]) + bytes([0x41 + (seq % 26)]) * (size - 1)


async def report(b, timeout=5):
    """Ask the device what it saw; returns (count, [(len, seq), ...])."""
    got = await b.send_lua(LUA_REPORT, await_print=True, timeout=timeout)
    parts = got.split()
    n = int(parts[0])
    seen = [tuple(int(x) for x in p.split("/")) for p in parts[1:]]
    await b.send_lua(LUA_RESET)
    return n, seen


def expect(label, n, seen, count, size):
    """All `count` packets of `size` bytes arrived, once each, in order."""
    want = [(size, i) for i in range(count)]
    check(n == count, f"{label}: {count} callbacks (got {n})")
    check(seen == want, f"{label}: sizes/order intact" + ("" if seen == want else f" (got {seen})"))


async def responsive(b, timeout=5):
    try:
        token = await b.send_lua("print('alive_'..tostring(2*21))", await_print=True, timeout=timeout)
        return token is not None and "alive_42" in token
    except Exception:
        return False


async def install(b):
    """Fresh VM with the counting callback registered and main.lua stopped."""
    await b.send_reset_signal()
    await asyncio.sleep(1)
    await b.send_break_signal()
    await asyncio.sleep(0.3)
    await b.send_lua(LUA_SETUP)
    await b.send_lua("frame.bluetooth.receive_callback(cb)")


async def main():
    ap = argparse.ArgumentParser(description="receive_callback delivery under load")
    ap.add_argument("--name", required=True, help='BLE device name, e.g. "Halo AB"')
    args = ap.parse_args()

    b = BrilliantBle()
    await b.connect(
        name=args.name,
        print_response_handler=lambda s: None,
        data_response_handler=lambda d: None,
    )
    print(f"Connected to {args.name}, max data payload {b.max_data_payload()} bytes")

    try:
        await install(b)

        # 1. Control: sequential, with response, idle REPL
        n_pk, size = 8, 60
        for i in range(n_pk):
            await b.send_data(pkt(i, size))
        await asyncio.sleep(0.5)
        expect("idle sequential", *await report(b), n_pk, size)

        # 2. Burst without response while idle
        n_pk, size = 32, 100
        for i in range(n_pk):
            await b.send_data(pkt(i, size), await_bt_response=False)
        await asyncio.sleep(1.0)
        expect("idle burst (no response)", *await report(b), n_pk, size)

        # 3. Burst while blocked in frame.sleep()
        n_pk, size = 16, 100
        await b.send_lua("frame.sleep(2)")
        await asyncio.sleep(0.2)
        for i in range(n_pk):
            await b.send_data(pkt(i, size))
        await asyncio.sleep(2.5)
        expect("burst during frame.sleep", *await report(b), n_pk, size)

        # 4. Burst while spinning in Lua
        n_pk, size = 16, 100
        await b.send_lua("local t=frame.time.utc(); while frame.time.utc()-t<2 do end")
        await asyncio.sleep(0.2)
        for i in range(n_pk):
            await b.send_data(pkt(i, size))
        await asyncio.sleep(2.5)
        expect("burst during busy loop", *await report(b), n_pk, size)

        # 5. Maximum-size frames
        n_pk, size = 8, b.max_data_payload()
        for i in range(n_pk):
            await b.send_data(pkt(i, size))
        await asyncio.sleep(1.0)
        expect(f"max-size frames ({size} B)", *await report(b), n_pk, size)

        # 6. Back-pressure: 40 x 200 B = 8 KB into a 4 KB queue while asleep.
        #    Each write-with-response either succeeds or raises; count them.
        n_pk, size = 40, 200
        await b.send_lua("frame.sleep(3)")
        await asyncio.sleep(0.2)
        accepted = 0
        refused = 0
        for i in range(n_pk):
            try:
                await b.send_data(pkt(i, size))
                accepted += 1
            except Exception:
                refused += 1
                # Once one is refused the ring is full; back off slightly.
                await asyncio.sleep(0.05)
        await asyncio.sleep(3.5)
        n, seen = await report(b)
        print(f"  back-pressure: accepted {accepted}, refused {refused}, delivered {n}")
        check(refused > 0, "back-pressure: overflow writes were refused, not dropped")
        check(n == accepted, f"back-pressure: every accepted write delivered ({n}/{accepted})")
        check(all(ln == size for ln, _ in seen), "back-pressure: frame sizes intact")
        seqs = [sq for _, sq in seen]
        check(seqs == sorted(seqs), "back-pressure: arrival order intact")
        check(await responsive(b), "back-pressure: device responsive afterwards")

        # 7. Ctrl+C with a burst queued behind a busy script. The break
        #    clears the callback (INTERRUPT semantics), so queued frames are
        #    discarded; what matters is that the break lands promptly.
        await b.send_lua(LUA_RESET)
        await b.send_lua("while true do end")
        await asyncio.sleep(0.2)
        for i in range(8):
            await b.send_data(pkt(i, 60))
        await b.send_break_signal()
        await asyncio.sleep(0.5)
        check(await responsive(b), "Ctrl+C lands with data burst pending")

        # 8. Ctrl+D (restart) while busy: the VM must restart, not wait.
        await b.send_lua("while true do end")
        await asyncio.sleep(0.2)
        await b.send_reset_signal()
        await asyncio.sleep(1.0)
        await b.send_break_signal()
        await asyncio.sleep(0.3)
        check(await responsive(b), "Ctrl+D restarts a busy VM")

        # 9. No callback registered: packets are discarded (queue must not
        #    fill), then a fresh registration receives new packets.
        await b.send_lua(LUA_SETUP)
        await b.send_lua("frame.bluetooth.receive_callback(nil)")
        await b.send_lua("frame.sleep(1)")
        await asyncio.sleep(0.2)
        for i in range(30):
            await b.send_data(pkt(i, 200))
        await asyncio.sleep(1.5)
        n, _ = await report(b)
        check(n == 0, f"no callback: packets discarded (got {n} callbacks)")
        await b.send_lua("frame.bluetooth.receive_callback(cb)")
        for i in range(8):
            await b.send_data(pkt(i, 60))
        await asyncio.sleep(0.5)
        expect("re-registered callback", *await report(b), 8, 60)

    finally:
        # Unregister only while the link is still up; on a dropped link these
        # calls raise from inside finally and replace the real failure.
        if b.is_connected():
            try:
                await b.send_lua("frame.bluetooth.receive_callback(nil)")
            except Exception as e:
                print(f"could not clear receive callback: {e}")
        await safe_teardown(b)

    if FAILURES:
        print(f"\n{len(FAILURES)} check(s) FAILED:")
        for f in FAILURES:
            print(f"  - {f}")
        raise SystemExit(1)
    print("\nAll checks passed")


if __name__ == "__main__":
    asyncio.run(main())
