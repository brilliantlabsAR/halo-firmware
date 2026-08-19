#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9,<3.13"
# dependencies = [
#     "brilliant-ble",
# ]
# ///
"""
Measure the PDM (mic) and I2S (speaker) effective sample rates against the
device's crystal-referenced kernel clock, using the AEC clock probes
(clkmon in audio_aec.c, exposed via frame.microphone.diag('stats')).

The duplex loop runs entirely on-device: the speaker is fed silence PCM by
a Lua loop (frame.speaker.play back-pressures on the driver's block slab,
so the feed self-paces to the I2S clock and never gaps), while the mic
thread streams and counts on its own. BLE carries no audio either way, so
nothing about the transport can stretch either stream.

  uv run clock_rate_test.py --name "Halo AB" --seconds 60

Prints both effective rates, their ppm offsets from 16000 Hz, the relative
mic-vs-speaker skew, and the raw clock-tree registers for HWRM decoding.
"""

import argparse
import asyncio

from brilliant_ble import BrilliantBle

SR = 16000
BLOCK = 320  # 20ms mono frames per speaker feed


printed = []


def receive_print(s):
    printed.append(s)


STATS_FIELDS = [
    "mic_frames", "mic_us", "mic_runs",
    "ref_frames", "ref_us", "ref_runs",
    "pdm_popped", "pdm_dropped", "pdm_overflows", "pdm_isrs",
    "pdm_stat_max", "pdm_stat_gt8",
    "r_osc_ctrl", "r_pll_lock_ctrl", "r_pll_clk_sel", "r_cgu_clk_ena",
    "r_i2s0_ctrl", "r_he_clk_ena", "r_pdm_ctl0", "r_pdm_ctl1",
]

STATS_LUA = (
    "local s=frame.microphone.diag('stats') print("
    + "..' '..".join(f"s.{f}" for f in STATS_FIELDS)
    + ")"
)


async def get_stats(b):
    printed.clear()
    await b.send_lua(STATS_LUA, await_print=True, timeout=10)
    vals = printed[-1].split()
    assert len(vals) == len(STATS_FIELDS), f"bad stats reply: {printed[-1]!r}"
    return dict(zip(STATS_FIELDS, (float(v) for v in vals)))


def report(tag, st):
    print(f"\n--- {tag} ---")
    for name in ("mic", "ref"):
        frames = st[f"{name}_frames"]
        us = st[f"{name}_us"]
        runs = int(st[f"{name}_runs"])
        if us > 0:
            rate = frames * 1e6 / us
            ppm = (rate / SR - 1.0) * 1e6
            print(f"{name}: {frames:.0f} frames / {us/1e6:.3f}s active "
                  f"({runs} runs) -> {rate:.2f} Hz ({ppm:+.0f} ppm)")
        else:
            print(f"{name}: no activity ({runs} runs)")
    if st["mic_us"] > 0 and st["ref_us"] > 0:
        mic_rate = st["mic_frames"] * 1e6 / st["mic_us"]
        ref_rate = st["ref_frames"] * 1e6 / st["ref_us"]
        print(f"mic/ref ratio: {mic_rate/ref_rate:.6f} "
              f"({(mic_rate/ref_rate - 1) * 1e6:+.0f} ppm)")
    delivered = st["pdm_popped"] - st["pdm_dropped"]
    print(f"pdm ledger: popped {st['pdm_popped']:.0f}, "
          f"dropped {st['pdm_dropped']:.0f}, delivered {delivered:.0f}, "
          f"overflows {int(st['pdm_overflows'])}, "
          f"isrs {int(st['pdm_isrs'])}, "
          f"stat_max {int(st['pdm_stat_max'])}, "
          f"stat_gt8 {int(st['pdm_stat_gt8'])}")
    if st["mic_us"] > 0:
        print(f"pdm delivered rate: {delivered * 1e6 / st['mic_us']:.2f} Hz "
              f"(vs mic thread {st['mic_frames'] * 1e6 / st['mic_us']:.2f} Hz)")
    print("registers:")
    for f in STATS_FIELDS:
        if f.startswith("r_"):
            print(f"  {f[2:]:>14s} = 0x{int(st[f]):08X}")


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    ap.add_argument("--seconds", type=int, default=60)
    args = ap.parse_args()

    b = BrilliantBle()
    await b.connect(name=args.name)
    b._user_print_response_handler = receive_print
    try:
        await b.send_break_signal()
        await asyncio.sleep(0.5)
        # make sure nothing from a previous session is still streaming
        await b.send_lua("pcall(frame.speaker.stop) "
                         "pcall(frame.microphone.stop) "
                         "frame.microphone.aec(false) print('reset')",
                         await_print=True, timeout=10)

        # register dump before any audio session
        pre = await get_stats(b)
        report("idle (registers)", pre)

        blocks = args.seconds * SR // BLOCK

        # Phase 1: mic only. The mic thread runs on its own; the Lua loop
        # just drains the ring (an undrained ring floods the UART with
        # discard warnings hard enough to wedge the device, seen
        # 2026-07-11) and yields to BLE.
        print(f"\nphase 1: mic only, {args.seconds}s...")
        printed.clear()
        await b.send_lua(
            "frame.microphone.diag('zero') "
            f"frame.microphone.start{{sample_rate={SR}, channels=1}} "
            f"for i=1,{blocks} do "
            "frame.microphone.read(4080) frame.sleep(0.02) end "
            "frame.microphone.stop() print('done')",
            await_print=True, timeout=2 * args.seconds + 60)
        report(f"mic only, {args.seconds}s", await get_stats(b))

        # Phase 2: speaker only, self-paced silence. play() blocks on the
        # driver's block slab, so the loop runs at the I2S clock rate and
        # the tap sees a gap-free stream.
        print(f"\nphase 2: speaker only, {args.seconds}s...")
        printed.clear()
        await b.send_lua(
            "frame.microphone.diag('zero') "
            f"frame.speaker.start{{sample_rate={SR}, channels=1, volume=20}} "
            f"local blk=string.rep('\\0', {BLOCK * 2}) "
            f"for i=1,{blocks} do frame.speaker.play(blk) "
            "if i % 100 == 0 then frame.sleep(0.001) end end "
            "frame.speaker.stop() print('done')",
            await_print=True, timeout=2 * args.seconds + 60)
        report(f"speaker only, {args.seconds}s", await get_stats(b))

        # Phase 3: duplex - both at once, same window, same kernel clock.
        print(f"\nphase 3: duplex, {args.seconds}s...")
        printed.clear()
        await b.send_lua(
            "frame.microphone.diag('zero') "
            f"frame.speaker.start{{sample_rate={SR}, channels=1, volume=20}} "
            f"frame.microphone.start{{sample_rate={SR}, channels=1}} "
            f"local blk=string.rep('\\0', {BLOCK * 2}) "
            f"for i=1,{blocks} do "
            "frame.speaker.play(blk) frame.microphone.read(4080) "
            "if i % 100 == 0 then frame.sleep(0.001) end end "
            "frame.speaker.stop() frame.microphone.stop() print('done')",
            await_print=True, timeout=2 * args.seconds + 60)
        report(f"duplex, {args.seconds}s", await get_stats(b))
    finally:
        await b.send_reset_signal()
        await b.disconnect()


asyncio.run(main())
