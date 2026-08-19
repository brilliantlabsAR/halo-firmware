#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9,<3.13"
# dependencies = [
#     "brilliant-ble",
#     "numpy",
# ]
# ///
# Note: pinned <3.13 because brilliant-ble -> bleak 0.22.3 -> pyobjc has no
# wheels for Python 3.13+ (uv would try to build it and fail). uv will fetch a
# compatible interpreter automatically.
"""
Record raw PCM to the Halo's on-device filesystem, then retrieve it and save a WAV.

Unlike test_microphone.py (which streams PCM live over BLE), this captures to a
file on the device first, so **BLE bandwidth is out of the loop during capture** —
and it's raw PCM, so **LC3 is out of the loop too**. The retrieval afterwards reads
the stored file back at whatever pace BLE allows (no real-time pressure), so it's
lossless.

Purpose: if the on-device raw *stereo* PCM is
clean, the demux/front-end is fine and the buzz is in the LC3 stereo encode. If it
has bleed/beats, the problem is upstream of LC3.

Requires the binary file read `f:read(n)` added to lua_file.c.

  uv run record_mic_lfs.py                              # 16 kHz stereo, 5 s
  uv run record_mic_lfs.py --rate 8000 --seconds 5     # lighter flash-write load
  uv run record_mic_lfs.py --channels 1 --out mono.wav
  uv run record_mic_lfs.py --name "Halo AB"            # target a specific device
"""

import argparse
import asyncio
import wave
from datetime import datetime

import numpy as np
from brilliant_ble import BrilliantBle

rx = bytearray()


def receive_data(data_view):
    # data_view is a memoryview of the notification (0x01 data marker stripped).
    rx.extend(bytes(data_view))
    print(f"Received {len(rx)} bytes", end="\r")


async def main(args):
    rate, ch, secs = args.rate, args.channels, args.seconds
    requested = int(rate * ch * 2 * secs)  # bytes: rate * channels * 2 (int16) * secs

    # SAFETY CAP. The /lfs storage partition is only 128 KB and the Zephyr log
    # backend writes to it too (CONFIG_LOG_BACKEND_FS_DIR="/lfs"). Measured free
    # space after remove_all() is ~119 KB (~1.86 s @ 16 k stereo) before ENOSPC.
    # Filling to 100% is still undesirable even though it no longer hangs the
    # device (the FS-log-backend fix in patches/zephyr.patch made a full disk
    # non-fatal): it stalls on slow near-full LittleFS writes, drops all logs, and
    # leaves no room for settings/bonds. So cap with headroom below the brink.
    SAFE_LFS_BYTES = 100000  # ~1.56 s @ 16 k stereo; ~19 KB headroom under ENOSPC
    target = min(requested, SAFE_LFS_BYTES)
    if target < requested:
        print(f"NOTE: clamping {requested} B ({secs:g}s) -> {target} B "
              f"({target/(rate*ch*2):.2f}s) to stay under /lfs capacity "
              f"(~119 KB brink; avoids slow near-full writes and lost logs).")
    fname = "mic_cap.pcm"

    b = BrilliantBle()
    await b.connect(name=args.name, data_response_handler=receive_data)

    # Clean slate + stop any running mic.
    await b.send_break_signal()
    await asyncio.sleep(0.5)
    await b.send_lua("frame.microphone.stop()")
    await asyncio.sleep(0.3)
    await b.send_lua("frame.file.remove_all()")
    await asyncio.sleep(0.3)

    # On-device capture loop: mic -> file until target bytes, then stop + close.
    # frame.microphone.read is NON-BLOCKING (returns '' when empty), so we
    # frame.sleep() every iteration to yield to the BLE thread — otherwise the
    # tight spin starves the link and it drops (~1s supervision timeout). Use a
    # small POSITIVE sleep: frame.sleep(0)/no-arg triggers deep sleep/shutdown.
    await b.send_lua(
        f"frame.microphone.start{{sample_rate={rate},bit_depth=16,channels={ch}}}")
    # Drain a big chunk per iteration (read is non-blocking, returns up to N or ''):
    # a small cap (e.g. 360 B) has almost no margin over the 64 KB/s stereo rate, so
    # any write stall lets the mic ring overflow ("Ring buffer full") and never
    # recovers. A large cap lets one read absorb the whole backlog after a stall.
    # 4096 is the firmware max (frame.microphone.read: "Bytes exceeds maximum of
    # 4096", must be even) — larger throws and the capture never starts.
    read_chunk = 4096
    # f:write throws (luaL_error) on ENOSPC/partial write, so guard it with pcall:
    # when /lfs fills we break cleanly, still close the file, and print the byte
    # count actually written. That lets you set --seconds beyond the partition
    # limit to probe the true on-device maximum without an unclean abort. Note
    # LittleFS writes slow sharply as the partition fills (metadata compaction),
    # so the capture timeout below is deliberately generous.
    capture = (
        f"local f=frame.file.open('{fname}','w') local n=0 "
        f"while n<{target} do local s=frame.microphone.read({read_chunk}) "
        f"if s==nil then break end "
        f"if s~='' then if not pcall(function() f:write(s) end) then break end n=n+#s end "
        f"frame.sleep(0.005) end f:close() frame.microphone.stop() print(n)"
    )
    retrieve = (
        f"local f=frame.file.open('{fname}','r') "
        f"while true do local s=f:read(240) if s==nil then break end "
        f"while true do if(pcall(frame.bluetooth.send,s))then break end end end "
        f"f:close() print('done')"
    )
    limit = b.max_lua_payload()
    for name, s in (("capture", capture), ("retrieve", retrieve)):
        if len(s) > limit:
            raise SystemExit(f"{name} Lua string {len(s)}B exceeds max_lua_payload {limit}B")

    print(f"Recording {secs}s of {rate} Hz / {ch}ch PCM to on-device '{fname}' "
          f"(~{target} bytes)...")
    # Generous: near-full LittleFS writes stall hard, so filling to ENOSPC (to
    # probe the max) can take far longer than the audio duration.
    written = await b.send_lua(capture, await_print=True, timeout=max(secs + 25, 150))
    print(f"  device wrote {written} bytes")

    print("Retrieving file over BLE (binary-safe read)...")
    rx.clear()
    await b.send_lua(retrieve, await_print=True, timeout=secs * 8 + 30)
    await asyncio.sleep(0.5)
    await b.disconnect()

    if not rx:
        print("\nNo data retrieved.")
        return

    # Trim to a whole number of interleaved frames and save.
    data = bytes(rx)
    audio = np.frombuffer(data[: len(data) - (len(data) % (2 * ch))], dtype=np.int16)
    out = args.out or f"lfs_pcm_{rate}hz_{ch}ch_{datetime.now():%Y%m%d_%H%M%S}.wav"
    with wave.open(out, "wb") as wf:
        wf.setnchannels(ch)
        wf.setsampwidth(2)
        wf.setframerate(rate)
        wf.writeframes(audio.tobytes())
    print(f"\nSaved {out}  ({len(data)} bytes, {len(data)/(rate*ch*2):.2f}s)")


def parse_args():
    p = argparse.ArgumentParser(
        description="Record raw PCM to the Halo filesystem, retrieve it, save a WAV.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--rate", type=int, default=16000, choices=[8000, 16000])
    p.add_argument("--channels", type=int, default=2, choices=[1, 2])
    p.add_argument("--seconds", type=float, default=5.0)
    p.add_argument("--name", default=None, help='Device name, e.g. "Halo AB"')
    p.add_argument("--out", default=None, help="Output WAV path")
    return p.parse_args()


if __name__ == "__main__":
    asyncio.run(main(parse_args()))
