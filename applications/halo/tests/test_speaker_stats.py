#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10,<3.13"
# dependencies = ["brilliant-ble>=3.3.0,<4", "lc3py", "numpy"]
# ///
"""On-device check of frame.speaker.stats(): drives each way speaker audio
can go missing and checks that the matching counter moves.

  uv run test_speaker_stats.py --name "Halo EC"

Cases: clean LC3 stream (every frame decoded and played), the same config
re-sent mid-stream (in-place update), a budget change mid-stream (restart),
garbage LC3 (PLC then mute), writes that are not whole frames, a full BLE
ring while the speaker is stopped and its flush at stop(), audio sent before
start(), PCM via frame.speaker.play(), and a start() with a bad argument on
a running stream.
"""
import argparse
import asyncio
import os

import lc3
import numpy as np
from brilliant_ble import BrilliantBle

SR, FB, FS = 16000, 40, 160
START = ("frame.speaker.start{encoder='lc3', sample_rate=16000, channels=1, "
         "duration=1000, bitrate=32000, volume=30}")

lines = []


def on_print(s):
    lines.append(s)


async def stats(b, reset=True):
    """frame.speaker.stats() as a dict; printed in short lines (the print
    buffer drops long ones)."""
    lines.clear()
    await b.send_lua(
        f"local t=frame.speaker.stats({'true' if reset else 'false'}) "
        "local o='' for k,v in pairs(t) do o=o..k..'='..tostring(v)..' ' "
        "if #o>120 then print(o) o='' end end print(o..'#end')")
    for _ in range(100):
        if lines and lines[-1].endswith("#end"):
            break
        await asyncio.sleep(0.05)
    d = {}
    for ln in lines:
        for kv in ln.replace("#end", "").split():
            k, v = kv.split("=", 1)
            d[k] = v == "true" if v in ("true", "false") else int(v)
    return d


def tone_lc3(seconds):
    t = np.arange(int(seconds * SR)) / SR
    pcm = (0.3 * 32767 * np.sin(2 * np.pi * 440 * t)).astype(np.int16)
    enc = lc3.Encoder(10000, SR, 1)
    return b"".join(enc.encode(pcm[i:i + FS].tobytes(), FB, bit_depth=16)
                    for i in range(0, len(pcm) - FS + 1, FS))


async def stream(b, data, per_write=10, pace=0.9):
    """Real-time-ish paced writes of whole frames."""
    step = FB * per_write
    for i in range(0, len(data), step):
        await b.send_audio(data[i:i + step], await_bt_response=False)
        await asyncio.sleep(per_write * 0.01 * pace)


results = []


def check(name, ok, detail):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'} {name}: {detail}")


async def main(args):
    b = BrilliantBle()
    await b.connect(name=args.name)
    b._user_print_response_handler = on_print
    await b.send_break_signal()
    await asyncio.sleep(0.5)
    fw = await b.send_lua("print(frame.FIRMWARE_VERSION)", await_print=True)
    print(f"firmware {fw}")
    await b.send_lua("frame.speaker.stop()")
    await stats(b)

    clip = tone_lc3(2.0)
    n = len(clip) // FB

    print("1. clean stream")
    await b.send_lua(START)
    await stream(b, clip)
    await asyncio.sleep(0.6)
    s = await stats(b)
    check("bytes arrive", s["ble_bytes"] == len(clip) and s["ble_rejected"] == 0,
          f"ble_bytes {s['ble_bytes']}/{len(clip)} rejected {s['ble_rejected']}")
    check("every frame decoded", s["frames_decoded"] == n,
          f"frames_decoded {s['frames_decoded']}/{n}")
    check("every frame played", s["blocks_played"] == n,
          f"blocks_played {s['blocks_played']}/{n}")
    loss = {k: s[k] for k in ("frames_plc", "frames_muted", "decode_errors",
                              "bytes_misaligned", "frames_dropped_stop",
                              "frames_write_failed", "blocks_discarded")}
    check("no losses", not any(loss.values()), loss)
    check("one start", s["starts"] == 1 and s["restarts"] == 0,
          f"starts {s['starts']} restarts {s['restarts']}")

    print("2a. same config re-sent mid-stream (backlog in the ring)")
    for i in range(0, len(clip), 400):
        await b.send_audio(clip[i:i + 400], await_bt_response=False)
        await asyncio.sleep(0.02)   # ~5x real time: builds a backlog
        if i == 400 * 10:
            await b.send_lua(START.replace("volume=30", "volume=25, gain=3"))
    await asyncio.sleep(2.5)
    s = await stats(b)
    check("updated in place", s["updates"] == 1 and s["restarts"] == 0 and
          s["starts"] == 0,
          f"updates {s['updates']} starts {s['starts']} "
          f"restarts {s['restarts']}")
    # frames_write_failed is the amp's -ENOSPC on a burst after idle (seen
    # with or without the update); only losses from the update count here
    check("nothing lost to the update", s["frames_decoded"] == n and
          s["blocks_played"] + s["frames_write_failed"] == n and
          s["frames_dropped_stop"] == 0 and s["blocks_discarded"] == 0,
          f"decoded {s['frames_decoded']} played {s['blocks_played']}/{n} "
          f"dropped_stop {s['frames_dropped_stop']} "
          f"discarded {s['blocks_discarded']} "
          f"write_failed {s['frames_write_failed']}")

    print("2b. budget change mid-stream restarts (backlog in the ring)")
    for i in range(0, len(clip), 400):
        await b.send_audio(clip[i:i + 400], await_bt_response=False)
        await asyncio.sleep(0.02)
        if i == 400 * 10:
            await b.send_lua(START.replace("volume=30", "volume=30, budget=50"))
    await asyncio.sleep(2.5)
    s = await stats(b)
    accounted = s["frames_decoded"] + s["frames_dropped_stop"]
    check("restart counted", s["restarts"] == 1 and s["starts"] == 1,
          f"starts {s['starts']} restarts {s['restarts']}")
    check("frames accounted", accounted == n,
          f"decoded {s['frames_decoded']} + dropped_stop "
          f"{s['frames_dropped_stop']} = {accounted}/{n}")
    lost = s["frames_decoded"] - s["blocks_played"]
    check("amp loss accounted", lost == s["blocks_discarded"] +
          s["frames_write_failed"],
          f"decoded-played {lost}, blocks_discarded {s['blocks_discarded']}, "
          f"write_failed {s['frames_write_failed']}, "
          f"drain_timeouts {s['drain_timeouts']}")

    print("3. garbage LC3")
    rng = np.random.default_rng(1)
    junk = rng.integers(0, 256, FB * 100, dtype=np.uint8).tobytes()
    await stream(b, junk)
    await asyncio.sleep(0.6)
    s = await stats(b)
    check("bad frames counted", s["frames_plc"] + s["frames_muted"] > 0
          and s["mute_events"] >= 1,
          f"plc {s['frames_plc']} muted {s['frames_muted']} "
          f"mute_events {s['mute_events']}")

    print("4. write that is not whole frames")
    # stop first: a same-config start() is an in-place update, and this
    # case needs a fresh, unmuted decoder
    await b.send_lua("frame.speaker.stop() " + START)
    await stats(b)
    await b.send_audio(clip[:FB + 1], await_bt_response=False)
    await asyncio.sleep(0.3)
    await b.send_audio(clip[FB + 1:2 * FB], await_bt_response=False)
    await asyncio.sleep(0.3)
    s = await stats(b)
    check("split frame joined", s["frames_decoded"] == 2 and
          s["bytes_misaligned"] == 0 and s["frames_plc"] == 0,
          f"frames_decoded {s['frames_decoded']}/2 "
          f"bytes_misaligned {s['bytes_misaligned']} plc {s['frames_plc']}")
    await b.send_audio(clip[:FB + 1], await_bt_response=False)
    await asyncio.sleep(0.3)
    await b.send_lua("frame.speaker.stop() " + START)
    await asyncio.sleep(0.2)
    s = await stats(b)
    check("tail at stop counted", s["bytes_misaligned"] == 1,
          f"bytes_misaligned {s['bytes_misaligned']}")

    print("4b. clip streamed in 244 B writes")
    await stats(b)
    for i in range(0, len(clip), 244):
        await b.send_audio(clip[i:i + 244], await_bt_response=False)
        await asyncio.sleep(244 / FB * 0.01 * 0.9)
    await asyncio.sleep(0.6)
    s = await stats(b)
    check("every frame decoded cleanly", s["frames_decoded"] == n and
          s["frames_plc"] == 0 and s["frames_muted"] == 0 and
          s["bytes_misaligned"] == 0,
          f"frames_decoded {s['frames_decoded']}/{n} plc {s['frames_plc']} "
          f"muted {s['frames_muted']} misaligned {s['bytes_misaligned']}")

    print("5. ring fills while the speaker is stopped")
    await b.send_lua("frame.speaker.stop()")
    await stats(b)
    big = clip * 3                 # 12 KB > 8 KB ring
    await stream(b, big, per_write=10, pace=0.1)
    await asyncio.sleep(0.3)
    s = await stats(b)
    check("rejections counted", s["ble_rejected"] > 0 and
          s["ble_bytes"] + s["ble_rejected_bytes"] == len(big),
          f"accepted {s['ble_bytes']} + rejected {s['ble_rejected_bytes']} "
          f"= {s['ble_bytes'] + s['ble_rejected_bytes']}/{len(big)}, "
          f"ring_bytes {s['ring_bytes']} ring_peak {s['ring_peak']}")
    check("audio kept for the next start", s["ring_bytes"] > 0 and
          not s["streaming"],
          f"ring_bytes {s['ring_bytes']} streaming {s['streaming']}")
    # start on the 8 KB backlog, then stop with most of it unplayed
    await b.send_lua(START)
    await asyncio.sleep(0.3)
    await b.send_lua("frame.speaker.stop()")
    await asyncio.sleep(0.2)
    s = await stats(b)
    # the pump's read in flight at stop counts as frames_dropped_stop
    used = (s["frames_decoded"] + s["frames_dropped_stop"]) * FB
    check("stop flushes the backlog", s["ring_bytes"] == 0 and
          s["ble_flushed_bytes"] > 0 and
          used + s["ble_flushed_bytes"] == 8000,
          f"ring_bytes {s['ring_bytes']} flushed {s['ble_flushed_bytes']} "
          f"+ (decoded {s['frames_decoded']} + dropped_stop "
          f"{s['frames_dropped_stop']})x{FB} = "
          f"{used + s['ble_flushed_bytes']}/8000")

    print("5b. audio written before start() plays from frame 0")
    await stream(b, clip[:FB * 50], pace=0.1)
    await asyncio.sleep(0.2)
    await b.send_lua(START)
    await asyncio.sleep(1.0)
    await b.send_lua("frame.speaker.stop()")
    s = await stats(b)
    check("early audio kept", s["frames_decoded"] == 50 and
          s["ble_flushed_bytes"] == 0 and s["frames_plc"] == 0,
          f"frames_decoded {s['frames_decoded']}/50 "
          f"flushed {s['ble_flushed_bytes']} plc {s['frames_plc']}")

    print("6. PCM via frame.speaker.play()")
    await stats(b)
    await b.send_lua(
        "frame.speaker.start{sample_rate=16000, bit_depth=16, volume=30} "
        "frame.speaker.play(string.rep('\\0\\0', 1600)) frame.speaker.stop()")
    await asyncio.sleep(0.5)
    s = await stats(b)
    check("pcm bytes counted", s["pcm_bytes"] == 3200 and
          s["pcm_bytes_failed"] == 0,
          f"pcm_bytes {s['pcm_bytes']} failed {s['pcm_bytes_failed']}")

    print("7. bad start() arguments leave a running stream alone")
    await b.send_lua(START)
    await stats(b)
    lines.clear()
    await b.send_lua(
        "local ok,e=pcall(frame.speaker.start,{encoder='lc3', "
        "sample_rate=16000, channels=1, duration=1000, bitrate=32000, "
        "volume=30, budget=150}) print(tostring(ok)..'|'..tostring(e))")
    for _ in range(40):
        if lines:
            break
        await asyncio.sleep(0.05)
    err = lines[0] if lines else ""
    await stream(b, clip[:FB * 50])
    await asyncio.sleep(0.6)
    s = await stats(b)
    check("bad budget rejected", err.startswith("false|") and "Budget" in err,
          err or "no reply")
    check("stream kept running", s["streaming"] and s["restarts"] == 0 and
          s["frames_decoded"] == 50,
          f"streaming {s['streaming']} restarts {s['restarts']} "
          f"frames_decoded {s['frames_decoded']}/50")
    await b.send_lua("frame.speaker.stop()")

    await b.send_reset_signal()
    await b.disconnect()
    print(f"\n{sum(results)}/{len(results)} checks passed")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default=None,
                    help='exact BLE device name, e.g. "Halo EC"')
    asyncio.run(main(ap.parse_args()))
