#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9,<3.13"
# dependencies = [
#     "brilliant-ble",
#     "lc3py",
#     "numpy",
# ]
# ///
# Note: pinned <3.13 for the same bleak/pyobjc reason as record_mic_lfs.py.
"""
On-device AEC loopback test, /lfs capture variant.

Like loopback_test.py, but the microphone LC3 is written to a file on the
device's /lfs during playback and retrieved afterwards - so BLE carries only
the downlink audio during the capture and the mic bytes are byte-exact (no
notification loss, no alignment guessing). LC3 mono is ~4 kB/s, so a 12 s
capture is ~48 kB, well under the ~100 kB safe /lfs budget (see
record_mic_lfs.py for the /lfs capacity notes this borrows from).

  uv run loopback_lfs.py --name "Halo AB" --wav story.wav --volume 100

Writes lfs_aec_off_<stamp>.wav / lfs_aec_on_<stamp>.wav for listening.
"""

import argparse
import asyncio
import wave
from datetime import datetime

import lc3
import numpy as np
from brilliant_ble import BrilliantBle

SR = 16000
BITRATE = 32000
FRAME_MS = 10
LC3_FRAME_BYTES = BITRATE // 8 * FRAME_MS // 1000  # 40
PCM_FRAME_SAMPLES = SR * FRAME_MS // 1000  # 160
FRAMES_PER_PACKET = 6  # 240B packets: safe multiple of 40 under any MTU

rx = bytearray()
printed = []


def receive_data(dv):
    rx.extend(bytes(dv))


def load_stimulus(path, seconds):
    with wave.open(path, "rb") as w:
        assert w.getframerate() == SR and w.getnchannels() == 1
        x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
    x = x.astype(np.float64) / 32768.0
    if seconds and len(x) > seconds * SR:
        x = x[: seconds * SR]
    return x


def encode_lc3(x):
    enc = lc3.Encoder(FRAME_MS * 1000, SR, 1)
    pcm = (np.clip(x, -1, 1) * 32767).astype(np.int16)
    out = bytearray()
    for i in range(len(pcm) // PCM_FRAME_SAMPLES):
        out += enc.encode(pcm[i * PCM_FRAME_SAMPLES:(i + 1) * PCM_FRAME_SAMPLES]
                          .tobytes(), LC3_FRAME_BYTES, bit_depth=16)
    return bytes(out)


def decode_lc3(data):
    dec = lc3.Decoder(FRAME_MS * 1000, SR, 1)
    out = [np.frombuffer(dec.decode(data[i:i + LC3_FRAME_BYTES], bit_depth=16),
                         dtype=np.int16)
           for i in range(0, len(data) - LC3_FRAME_BYTES + 1, LC3_FRAME_BYTES)]
    return (np.concatenate(out).astype(np.float64) / 32768.0) if out else np.zeros(0)


def save_wav(path, x):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())


async def run_pass(b, lc3_clip, aec_on, volume, seconds, out_path):
    global rx
    target = seconds * BITRATE // 8  # LC3 bytes for the capture duration

    await b.send_lua(f"frame.microphone.aec({'true' if aec_on else 'false'})")
    if lc3_clip is not None:
        await b.send_lua(
            f"frame.speaker.start{{encoder='lc3', sample_rate={SR}, channels=1, "
            f"duration=1000, bitrate={BITRATE}, volume={volume}}}")
    await b.send_lua("frame.file.remove_all()")
    await asyncio.sleep(0.3)

    # capture mic LC3 to /lfs (single command; frame.sleep yields to BLE;
    # pcall-guarded writes; prints byte count when done)
    capture_fut = asyncio.ensure_future(b.send_lua(
        f"frame.microphone.start{{encoder='lc3', sample_rate={SR}, "
        f"bitrate={BITRATE}, channels=1}} "
        f"local f=frame.file.open('cap.lc3','w') local n=0 local buf='' "
        f"while n<{target} do local s=frame.microphone.read(4080) "
        f"if s==nil then break end "
        f"if s~='' then buf=buf..s end "
        f"if #buf>=1920 then if not pcall(function() f:write(buf) end) then break end n=n+#buf buf='' end "
        f"frame.sleep(0.01) end "
        f"if #buf>0 then pcall(function() f:write(buf) end) n=n+#buf end "
        f"f:close() frame.microphone.stop() print(n)",
        await_print=True, timeout=seconds + 30))

    await asyncio.sleep(1.0)  # lead-in

    if lc3_clip is not None:
        packet = LC3_FRAME_BYTES * FRAMES_PER_PACKET
        for i in range(0, len(lc3_clip), packet):
            await b.send_audio(lc3_clip[i:i + packet], await_bt_response=False)
            await asyncio.sleep(FRAME_MS * FRAMES_PER_PACKET / 1000 * 0.90)

    written = await capture_fut  # waits until the capture loop hits target
    if lc3_clip is not None:
        await b.send_lua("frame.speaker.stop()")
    print(f"  device captured {written} LC3 bytes")

    # retrieve the file (byte-exact; the only BLE uplink happens now)
    rx = bytearray()
    await b.send_lua(
        "local f=frame.file.open('cap.lc3','r') "
        "while true do local s=f:read(240) if s==nil then break end "
        "while true do if(pcall(frame.bluetooth.send,s))then break end end end "
        "f:close() print('sent')", await_print=True, timeout=120)
    await asyncio.sleep(0.5)
    data = bytes(rx)
    print(f"  retrieved {len(data)} bytes")

    with open(out_path.replace('.wav', '.lc3'), 'wb') as f:
        f.write(data)
    mic = decode_lc3(data)
    save_wav(out_path, mic)
    print(f"  {len(mic)/SR:.1f}s -> {out_path}")
    return mic


def score(off, on):
    # speech-band emphasis; also applied BEFORE envelope detection - the mic's
    # constant sub-100Hz rumble dominates full-band RMS and blinds the detector
    def bp(x):
        X = np.fft.rfft(x)
        f = np.fft.rfftfreq(len(x), 1 / SR)
        X[(f < 300) | (f > 3400)] = 0
        return np.fft.irfft(X, len(x))

    off = bp(off)
    on = bp(on)

    # the mic-start PDM pop occupies the first ~0.5s of every capture at
    # ~-28dBFS - never score it (playback starts at ~1s, after the lead-in)
    pop_guard = int(1.2 * SR)

    # locate playback via the louder (off) capture's envelope
    hop = SR // 5
    r = np.sqrt(np.mean(off[:len(off)//hop*hop].reshape(-1, hop)**2, axis=1))
    r[:pop_guard // hop + 1] = 0
    hot = r > max(np.percentile(r[pop_guard // hop + 1:], 20) * 3, 1e-5)
    # bridge speech pauses (up to 1s) so playback reads as one run, then take
    # the longest run - isolated hot bins elsewhere are transients (the
    # mic-stop click puts one in the very last bin)
    gap = SR // hop  # 1s of bins
    idx = np.nonzero(hot)[0]
    for i, j in zip(idx[:-1], idx[1:]):
        if j - i <= gap:
            hot[i:j] = True
    runs = []
    start = None
    for i, h in enumerate(np.append(hot, False)):
        if h and start is None:
            start = i
        elif not h and start is not None:
            runs.append((i - start, start, i))
            start = None
    if not runs or max(runs)[0] < 3:
        print("no playback region found in the AEC-off capture")
        return
    _, i0, i1 = max(runs)
    a, b_ = max((i0 + 1) * hop, pop_guard), i1 * hop
    n = min(len(off), len(on), b_)

    def erle_of(x0, x1):
        return (10 * np.log10((x0**2).mean() + 1e-15),
                10 * np.log10((x1**2).mean() + 1e-15),
                10 * np.log10((x0**2).mean() / ((x1**2).mean() + 1e-15)))

    od, cd, erle = erle_of(off[a:n], on[a:n])
    print(f"\nplayback region {a/SR:.1f}-{n/SR:.1f}s (PDM pop excluded)")
    print(f"AEC off: {od:+.1f} dB   AEC on: {cd:+.1f} dB   ERLE: {erle:+.1f} dB")

    # split out the adaptation onset (first 1.5s of playback) so a divergence
    # burst there can't hide - or masquerade as - steady-state performance
    ss = a + int(1.5 * SR)
    if n - ss > SR:
        _, _, e_on = erle_of(off[a:ss], on[a:ss])
        _, _, e_ss = erle_of(off[ss:n], on[ss:n])
        print(f"onset ({a/SR:.1f}-{ss/SR:.1f}s): {e_on:+.1f} dB   "
              f"steady-state ({ss/SR:.1f}-{n/SR:.1f}s): {e_ss:+.1f} dB")


async def main(args):
    stim = load_stimulus(args.wav, args.seconds)
    lc3_clip = encode_lc3(stim)
    print(f"stimulus {len(stim)/SR:.1f}s")

    b = BrilliantBle()
    await b.connect(data_response_handler=receive_data, name=args.name)
    print("connected")

    capture_secs = int(len(stim) / SR) + 4
    stamp = datetime.now().strftime("%H%M%S")
    await run_pass(b, None, False, args.volume, 6,
                   f"lfs_miconly_{stamp}.wav")
    off = await run_pass(b, lc3_clip, False, args.volume, capture_secs,
                         f"lfs_aec_off_{stamp}.wav")
    on = await run_pass(b, lc3_clip, True, args.volume, capture_secs,
                        f"lfs_aec_on_{stamp}.wav")
    await b.send_lua("frame.file.remove_all()")
    await b.disconnect()
    score(off, on)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default=None)
    ap.add_argument("--wav", required=True)
    ap.add_argument("--seconds", type=int, default=10,
                    help="stimulus length; the default plays all of the 10s "
                         "loopback_stim.wav instead of truncating mid-sentence")
    ap.add_argument("--volume", type=int, default=100)
    asyncio.run(main(ap.parse_args()))
