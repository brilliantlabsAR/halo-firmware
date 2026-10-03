#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10,<3.13"
# dependencies = ["brilliant-ble>=3.3.0,<4", "lc3py", "numpy"]
# ///
"""On-device check of frame.speaker.stats(): drives each way speaker audio
can go missing and checks that the matching counter moves.

  uv run test_speaker_stats.py --name "Halo EC"

Cases: clean LC3 stream (every frame decoded and played), a restart mid-
stream, garbage LC3 (PLC then mute), a write that is not whole frames, a
full BLE ring while the speaker is stopped, and PCM via frame.speaker.play().
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

    print("2. restart mid-stream (backlog in the ring)")
    for i in range(0, len(clip), 400):
        await b.send_audio(clip[i:i + 400], await_bt_response=False)
        await asyncio.sleep(0.02)   # ~5x real time: builds a backlog
        if i == 400 * 10:
            await b.send_lua(START)
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
    await b.send_lua(START)       # fresh decoder, unmuted
    await stats(b)
    await b.send_audio(clip[:FB + 1], await_bt_response=False)
    await asyncio.sleep(0.5)
    s = await stats(b)
    check("misaligned tail counted", s["bytes_misaligned"] == 1,
          f"bytes_misaligned {s['bytes_misaligned']} "
          f"frames_decoded {s['frames_decoded']}")

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
    check("stale audio visible", s["ring_bytes"] > 0 and not s["streaming"],
          f"ring_bytes {s['ring_bytes']} streaming {s['streaming']}")
    # drain the stale backlog so it does not leak into the next run
    await b.send_lua(START)
    await asyncio.sleep(s["ring_bytes"] / 4000 + 1.0)
    await b.send_lua("frame.speaker.stop()")

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

    await b.send_reset_signal()
    await b.disconnect()
    print(f"\n{sum(results)}/{len(results)} checks passed")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default=None,
                    help='exact BLE device name, e.g. "Halo EC"')
    asyncio.run(main(ap.parse_args()))
