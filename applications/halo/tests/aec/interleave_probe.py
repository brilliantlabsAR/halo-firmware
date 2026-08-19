#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9,<3.13"
# dependencies = [
#     "brilliant-ble",
# ]
# ///
"""Interleaved-burst margin probe: reproduce/validate the capture-loss
timeline slip WITHOUT the s2s server or a phone.

The 2026-07-13 duplex session died at ~100s because the mic-anchored
reference window slipped off the 256ms reference ring at ~36-72 samples/s.
The slip is a CAPTURE-side deficit: under duplex load (BLE mic streaming +
LC3 encode + playback interleaved with the driver silence-feed) the PDM
layer loses samples (FIFO overflow clears, queue-full drops), the AEC's
seen-samples timeline falls behind real time, and the drift check - both
operands clocked by seen samples - never fires. Pure silence-feed (idle
CPU) measured sample-exact, so the loss needs load to reproduce.

This probe recreates the load pattern locally: an open speaker session
(driver silence-feed active), short real playback bursts every ~2s
(real->silence->real handoffs through the DMA queue), the mic running
with LC3 encode and its frames pushed over BLE (TX load), while
aec('stats') is sampled between 2s segments - margin(t) and the loss
ledger as a TIME SERIES.

Verdict on a fixed build: pdm_dropped/pdm_overflows may still tick under
load, but mic_lost tracks pdm_dropped, cap_slips absorbs the rest, and
margin_last stays bounded (a few hundred samples, flat trend). On a
pre-fix build margin_last grows monotonically and never comes back.

  uv run interleave_probe.py --name "Halo AB" --seconds 120
"""

import argparse
import asyncio
import json
import time

from brilliant_ble import BrilliantBle

SR = 16000
BLOCK = 320          # 20ms mono frames per speaker feed
BURST_BLOCKS = 15    # 300ms real playback burst per segment
SEG_MIC_ITERS = 170  # ~1.7s of mic pump per segment

printed = []


def receive_print(s):
    printed.append(s)


STATS_FIELDS = [
    "margin_last", "margin_min", "margin_max",
    "resyncs", "ref_underruns", "ref_pads",
    "feed_gaps", "feed_gap_ms",
    "mic_lost", "cap_slips", "cap_slip_ms", "cap_late_floor",
    "emit_slips", "emit_slip_ms",
    "mic_frames", "mic_us", "mic_runs",
    "ref_frames", "ref_us", "ref_runs",
    "pdm_popped", "pdm_dropped", "pdm_discarded", "pdm_overflows",
    "pdm_isrs", "pdm_stat_max", "pdm_gap_max_us", "pdm_gap_over",
    "pdm_lost_est",
    "spk_real_sends", "spk_silence_sends", "spk_err_completions",
    "spk_tap_drops", "spk_cb_send_fails",
]

# two prints per read: a single ~280-char print of every field overflows
# the print buffer and silently drops the tail (the 2026-07-13 session
# dump lost pdm_dropped to exactly this failure mode)
_HALF = len(STATS_FIELDS) // 2
STATS_LUA_A = (
    "local s=frame.microphone.diag('stats') print("
    + "..' '..".join(f"s.{f}" for f in STATS_FIELDS[:_HALF]) + ")"
)
STATS_LUA_B = (
    "local s=frame.microphone.diag('stats') print("
    + "..' '..".join(f"s.{f}" for f in STATS_FIELDS[_HALF:]) + ")"
)


async def get_stats(b):
    printed.clear()
    await b.send_lua(STATS_LUA_A, await_print=True, timeout=10)
    vals = printed[-1].split()
    await b.send_lua(STATS_LUA_B, await_print=True, timeout=10)
    vals += printed[-1].split()
    assert len(vals) == len(STATS_FIELDS), f"bad stats reply: {printed!r}"
    return dict(zip(STATS_FIELDS, (float(v) for v in vals)))


# One ~2s segment, single Lua line: a 300ms tone burst through the real
# write path (real->silence->real handoff either side), then ~1.7s of
# LC3 mic pump with the frames pushed over BLE (the duplex TX load).
# The speaker and mic sessions stay open across segments - the ~50ms
# REPL gap between send_lua calls is bridged by the driver silence-feed
# (speaker) and the mic ring, and stays far under the AEC's 200ms mic
# session-reset threshold.
SEG_LUA = (
    f"for i=1,{BURST_BLOCKS} do frame.speaker.play(burst) end "
    f"for i=1,{SEG_MIC_ITERS} do "
    "local d=frame.microphone.read(240) "
    "if d and d~='' then pcall(frame.bluetooth.send,'\\x05'..d) end "
    "frame.sleep(0.01) end print('seg')"
)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    ap.add_argument("--seconds", type=int, default=120)
    ap.add_argument("--volume", type=int, default=40)
    ap.add_argument("--out", default=None,
                    help="jsonl path for the per-segment stats series")
    args = ap.parse_args()

    out_path = args.out or f"interleave_{time.strftime('%H%M%S')}.jsonl"
    segments = max(1, args.seconds // 2)

    b = BrilliantBle()
    await b.connect(name=args.name)
    b._user_print_response_handler = receive_print
    try:
        await b.send_break_signal()
        await asyncio.sleep(0.5)
        await b.send_lua("pcall(frame.speaker.stop) "
                         "pcall(frame.microphone.stop) print('reset')",
                         await_print=True, timeout=10)

        # sessions open for the whole probe; burst = 20ms of a ~500Hz
        # square-ish tone at low level, repeated per play()
        await b.send_lua(
            "frame.microphone.aec(true) frame.microphone.diag('zero') "
            f"frame.speaker.start{{sample_rate={SR}, channels=1, "
            f"volume={args.volume}}} "
            "frame.microphone.start{encoder='lc3', sample_rate=16000, "
            "bitrate=32000, channels=1} "
            "burst='' local hi=string.rep('\\x00\\x18',16) "
            "local lo=string.rep('\\x00\\xe8',16) "
            f"for i=1,10 do burst=burst..hi..lo end "
            "print('go')",
            await_print=True, timeout=10)

        t0 = time.time()
        rows = []
        prev = None
        with open(out_path, "w") as f:
            for seg in range(segments):
                printed.clear()
                await b.send_lua(SEG_LUA, await_print=True, timeout=30)
                st = await get_stats(b)
                st["t"] = round(time.time() - t0, 3)
                f.write(json.dumps(st) + "\n")
                f.flush()
                rows.append(st)
                d = ""
                if prev is not None:
                    d = (f"  d_dropped={st['pdm_dropped']-prev['pdm_dropped']:.0f}"
                         f" d_ovf={st['pdm_overflows']-prev['pdm_overflows']:.0f}"
                         f" d_lost={st['mic_lost']-prev['mic_lost']:.0f}")
                print(f"[{st['t']:7.1f}s] margin_last={st['margin_last']:6.0f}"
                      f" max={st['margin_max']:6.0f}"
                      f" resyncs={st['resyncs']:.0f}"
                      f" slips={st['cap_slips']:.0f}"
                      f" floor={st['cap_late_floor']:.0f}ms"
                      f" gap_max={st['pdm_gap_max_us']:.0f}us"
                      f" err_cmp={st['spk_err_completions']:.0f}{d}")
                prev = st

        # verdict: margin trend over the run
        if len(rows) >= 10:
            n = len(rows)
            m0 = sum(r["margin_last"] for r in rows[:n // 3]) / (n // 3)
            m1 = sum(r["margin_last"] for r in rows[-(n // 3):]) / (n // 3)
            span = rows[-1]["t"] - rows[0]["t"]
            slope = (m1 - m0) / (span * 2 / 3) if span else 0.0
            last = rows[-1]
            print(f"\nmargin trend: {m0:.0f} -> {m1:.0f} samples "
                  f"({slope:+.1f}/s); losses: pdm_dropped "
                  f"{last['pdm_dropped']:.0f}, overflows "
                  f"{last['pdm_overflows']:.0f}, mic_lost "
                  f"{last['mic_lost']:.0f}, cap_slips {last['cap_slips']:.0f} "
                  f"(+{last['cap_slip_ms']:.0f}ms), resyncs "
                  f"{last['resyncs']:.0f}")
            # a true runaway is monotone at the loss rate (~36-72/s on the
            # 003139 session); anchor wander from slides/resyncs is bounded
            # noise of a few hundred samples either way
            bounded = (abs(slope) < 15.0 and
                       all(abs(r["margin_last"]) < 1500 for r in rows))
            if bounded:
                print("VERDICT: margin bounded - no timeline slip")
            else:
                print("VERDICT: margin slipping - capture losses are not "
                      "being absorbed")
        print(f"series written to {out_path}")
    finally:
        try:
            await b.send_lua("pcall(frame.speaker.stop) "
                             "pcall(frame.microphone.stop) print('bye')",
                             await_print=True, timeout=5)
        except Exception:
            pass
        await b.send_reset_signal()
        await b.disconnect()


asyncio.run(main())
