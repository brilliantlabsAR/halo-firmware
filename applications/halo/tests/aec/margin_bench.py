#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9,<3.13"
# dependencies = [
#     "brilliant-ble",
# ]
# ///
"""
Bench oracle for the AEC reference-margin servo (AEC_MARGIN_SERVO), no wearing.

Runs an on-device duplex loop with the AEC ENABLED: the speaker emits silence
(so the I2S/emit clock runs and the tap feeds the reference ring gap-free) while
the mic streams (PDM/capture clock). Content is irrelevant - `margin` is a ring-
index relationship (write head vs mic-anchored read window), so the real
I2S-vs-PDM hardware clock skew walks it exactly as in a worn duplex reply, with
none of the acoustics or a server. The loop prints a margin time-series so the
servo's effect is visible directly:

  servo OFF -> margin drifts negative, ref_underruns / ref_pads climb
  servo ON  -> margin holds near its anchor, ref_underruns / ref_pads stay flat,
               ref_skew_adj ramps to track the skew

  uv run margin_bench.py --name "Halo AB" --seconds 180

LIMITATION (measured 2026-07-14 on the dev kit): this bench does NOT reproduce the
worn margin collapse and cannot on its own validate the servo. Two reasons:
(1) with a SILENCE reference the padding it triggers is silence-into-silence -
harmless, unlike the worn case where padding drops real echo reference; (2) in
this Lua-paced loop the dev kit showed NO net emit-vs-capture skew drift (margin pinned
constant -101 servo-off / -69 servo-on over 180s, ref_skew_adj stayed 0 - the
servo correctly idle with nothing to correct). Use it as a boot / duplex-
stability smoke test and to confirm the servo is inert on a skew-free stream;
the servo's benefit is shown by host/skew_sim.c (a faithful ppm-skew model) and
the final oracle is a worn Halo unit duplex run (real echo + the unit's real
~250ppm skew).
"""

import argparse
import asyncio

from brilliant_ble import BrilliantBle

SR = 16000
BLOCK = 320  # 20ms mono frames per speaker feed
EVERY = 250  # print stats every 250 blocks (~5s)

printed = []


def receive_print(s):
    printed.append(s)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    ap.add_argument("--seconds", type=int, default=180)
    ap.add_argument("--volume", type=int, default=20)
    args = ap.parse_args()

    blocks = args.seconds * SR // BLOCK

    b = BrilliantBle()
    await b.connect(name=args.name)
    b._user_print_response_handler = receive_print
    try:
        await b.send_break_signal()
        await asyncio.sleep(0.5)
        await b.send_lua("pcall(frame.speaker.stop) "
                         "pcall(frame.microphone.stop) print('reset')",
                         await_print=True, timeout=10)

        # AEC ON so audio_aec_process runs ref_consume() (and the servo);
        # zero the counters so margin/underruns start fresh.
        printed.clear()
        print(f"duplex (AEC on), {args.seconds}s, printing margin every "
              f"~{EVERY * BLOCK // SR}s...")
        # NB single send_lua must fit in mtu-3 (~509B); keep this compact.
        lua = (
            "local m=frame.microphone local sp=frame.speaker "
            "m.aec(true) m.diag('zero') "
            f"sp.start{{sample_rate={SR},channels=1,volume={args.volume}}} "
            f"m.start{{sample_rate={SR},channels=1}} "
            f"local b=string.rep('\\0',{BLOCK * 2}) "
            f"for i=1,{blocks} do sp.play(b) m.read(4080) "
            f"if i%{EVERY}==0 then local s=m.diag('stats') "
            "print('M '..i..' '..s.margin_last..' '..s.ref_underruns..' '.."
            "(s.ref_skew_adj or 0)..' '..s.resyncs..' '..s.ref_pads) end "
            "if i%100==0 then frame.sleep(0.001) end end "
            "sp.stop() m.stop() print('done')")
        assert len(lua) <= b.max_lua_payload(), \
            f"lua {len(lua)}B > max {b.max_lua_payload()}B"
        # fire-and-forget: the loop prints periodically and the notification
        # handler (receive_print) accumulates every print; wait it out, then
        # parse, rather than blocking on a single await_print.
        await b.send_lua(lua, await_print=False)
        deadline = args.seconds + 20
        waited = 0
        while waited < deadline and not any(
                ln.strip() == "done" for ln in printed):
            await asyncio.sleep(2)
            waited += 2

        rows = [ln.split() for ln in printed if ln.startswith("M ")]
        print(f"\n{'t(s)':>5} {'margin':>7} {'underruns':>10} "
              f"{'skew_adj':>9} {'resyncs':>8} {'ref_pads':>9}")
        for r in rows:
            _, i, margin, und, adj, res, pads = r[:7]
            t = int(i) * BLOCK // SR
            print(f"{t:>5} {margin:>7} {und:>10} {adj:>9} {res:>8} {pads:>9}")
        if not rows:
            print("(no margin rows - check REPL output)")
            for ln in printed[-8:]:
                print("  ", repr(ln))
    finally:
        await b.send_reset_signal()
        await b.disconnect()


asyncio.run(main())
