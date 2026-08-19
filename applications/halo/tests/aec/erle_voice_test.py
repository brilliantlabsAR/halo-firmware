#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9,<3.13"
# dependencies = [
#     "brilliant-ble",
#     "lc3py",
#     "numpy",
# ]
# ///
"""
AEC ERLE measurement with a RECORDED ASSISTANT VOICE as the stimulus.

Same gap-free on-device methodology as erle_local_test.py, but instead
of Lua-synthesized white noise the device plays a real far-end clip
(e.g. a Gemini voice from a realtime_gemini capture): the clip is
LC3-encoded host-side, uploaded once into Lua RAM over the BLE data
channel, and played through the speaker's LC3 mode - whose play() call
uses the same blocking DMA back-pressure, so emission stays gap-free.

Speech is non-stationary, so besides the overall band ERLE the score is
also reported over voice-active frames only (off-capture band energy
above the floor), which is the number that matters for barge-in.

  uv run erle_voice_test.py --name "Halo AB" --volume 80 \\
      --clip <far-end reference wav>
"""

import argparse
import asyncio
from datetime import datetime
import wave

import lc3
import numpy as np
from brilliant_ble import BrilliantBle

SR = 16000
BITRATE = 32000
FRAME_MS = 10
LC3_FRAME_BYTES = BITRATE // 8 * FRAME_MS // 1000  # 40
CHUNK_FRAMES = 6                # 240 B per uploaded chunk / play() call
LEAD_S = 1.0
TAIL_S = 1.0

rx = bytearray()
printed = []


def receive_data(dv):
    rx.extend(bytes(dv))


def receive_print(s):
    printed.append(s)


def decode_lc3(data):
    dec = lc3.Decoder(FRAME_MS * 1000, SR, 1)
    out = [np.frombuffer(dec.decode(bytes(data[i:i + LC3_FRAME_BYTES]),
                                    bit_depth=16), dtype=np.int16)
           for i in range(0, len(data) - LC3_FRAME_BYTES + 1, LC3_FRAME_BYTES)]
    return (np.concatenate(out).astype(np.float64) / 32768.0) if out else np.zeros(0)


def save_wav(path, x):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())


def bandpass(x, lo=300.0, hi=3400.0):
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1.0 / SR)
    X[(f < lo) | (f > hi)] = 0
    return np.fft.irfft(X, n=len(x))


def rms_db(x):
    return 20 * np.log10(np.sqrt(np.mean(x ** 2)) + 1e-12)


def load_clip(path, seconds):
    """Best `seconds` slice of the clip by speech-band energy."""
    with wave.open(path) as w:
        assert w.getframerate() == SR and w.getnchannels() == 1
        pcm = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
    x = pcm.astype(np.float64) / 32768.0
    xb = bandpass(x) ** 2
    win = int(seconds * SR)
    if len(x) <= win:
        best = 0
    else:
        c = np.cumsum(xb)
        starts = np.arange(0, len(x) - win, SR // 2)
        best = int(starts[np.argmax(c[starts + win] - c[starts])])
    # round to whole LC3 frames
    sl = pcm[best:best + win]
    sl = sl[:len(sl) - len(sl) % (SR * FRAME_MS // 1000)]
    print(f"clip slice: {best / SR:.1f}s..{(best + len(sl)) / SR:.1f}s, "
          f"band RMS {rms_db(bandpass(sl / 32768.0)):+.1f} dBFS")
    return sl


def encode_clip(pcm):
    enc = lc3.Encoder(FRAME_MS * 1000, SR, 1)
    spf = SR * FRAME_MS // 1000
    frames = [enc.encode(pcm[i:i + spf].tobytes(), LC3_FRAME_BYTES,
                         bit_depth=16)
              for i in range(0, len(pcm), spf)]
    return b"".join(frames)


async def upload_clip(b, blob):
    await b.send_lua(
        "vc={} vn=0 frame.bluetooth.receive_callback("
        "function(d) vc[#vc+1]=d vn=vn+#d end) print('rx-ready')",
        await_print=True, timeout=10)
    chunk = CHUNK_FRAMES * LC3_FRAME_BYTES
    for i in range(0, len(blob), chunk):
        await b.send_data(bytearray(blob[i:i + chunk]))
    await asyncio.sleep(0.5)
    printed.clear()
    await b.send_lua("frame.bluetooth.receive_callback(nil) print(vn)",
                     await_print=True, timeout=10)
    got = int(printed[-1])
    print(f"uploaded {got}/{len(blob)} LC3 bytes in {len(blob) // chunk} chunks")
    if got != len(blob):
        raise RuntimeError("upload incomplete - rerun")


async def run_pass(b, aec_on, volume, seconds):
    global rx
    target = int((LEAD_S + seconds + TAIL_S) * BITRATE // 8)

    await b.send_lua(f"frame.microphone.aec({'true' if aec_on else 'false'})")
    await b.send_lua("frame.file.remove_all()")
    await asyncio.sleep(0.3)

    await b.send_lua(
        "capt={} capn=0 "
        "drainf=function() local s=frame.microphone.read(4080) "
        "if s and s~='' then capt[#capt+1]=s capn=capn+#s end end "
        "print('ok2')",
        await_print=True, timeout=10)
    await b.send_lua(
        f"frame.microphone.start{{encoder='lc3', sample_rate={SR}, "
        f"bitrate={BITRATE}, channels=1}} capt={{}} capn=0 "
        f"for i=1,{int(LEAD_S * 100)} do drainf() frame.sleep(0.01) end "
        "print('ok3')",
        await_print=True, timeout=30)
    await b.send_lua(
        f"frame.speaker.start{{encoder='lc3', sample_rate={SR}, channels=1, "
        f"duration=1000, bitrate={BITRATE}, volume={volume}}} "
        "for i=1,#vc do frame.speaker.play(vc[i]) drainf() end "
        "frame.speaker.stop() print('ok4')",
        await_print=True, timeout=seconds + 60)
    if printed[-1] != "ok4":
        raise RuntimeError(f"playback failed: {printed[-1]}")
    printed.clear()
    await b.send_lua(
        f"for i=1,{int((TAIL_S + 5) * 100)} do "
        f"if capn>={target} then break end drainf() frame.sleep(0.01) end "
        "frame.microphone.stop() "
        "local f=frame.file.open('cap.lc3','w') "
        "for i=1,#capt do pcall(function() f:write(capt[i]) end) end "
        "f:close() print(capn)",
        await_print=True, timeout=int(TAIL_S) + 60)
    print(f"  device captured {printed[-1]} LC3 bytes")

    if aec_on:
        printed.clear()
        await b.send_lua(
            "local s=frame.microphone.diag('stats') "
            "print(s.w_norm2..' '..s.resyncs..' '..s.ref_underruns..' '"
            "..s.ref_pads..' '..s.feed_gaps..' '..s.feed_gap_ms)",
            await_print=True, timeout=10)
        print(f"  aec stats (w_norm2 resyncs underruns pads gaps gap_ms): "
              f"{printed[-1]}")

    rx = bytearray()
    await b.send_lua(
        "local f=frame.file.open('cap.lc3','r') "
        "while true do local s=f:read(240) if s==nil then break end "
        "while true do if(pcall(frame.bluetooth.send,s))then break end end end "
        "f:close() print('sent')", await_print=True, timeout=240)
    await asyncio.sleep(0.5)
    data = bytes(rx)
    data = data[: len(data) - len(data) % LC3_FRAME_BYTES]
    print(f"  retrieved {len(data)} bytes")
    return decode_lc3(data)


def score(off, on, seconds):
    lead = int(LEAD_S * SR)
    ob = bandpass(off)[lead:lead + int(seconds * SR)]
    nb = bandpass(on)[lead:lead + int(seconds * SR)]
    n = min(len(ob), len(nb))
    ob, nb = ob[:n], nb[:n]

    floor_o = bandpass(off)[int(0.5 * SR):lead]
    print(f"\nAEC off: floor {rms_db(floor_o):+.1f} dBFS, "
          f"playback {rms_db(ob):+.1f} dBFS")
    print(f"AEC on:  playback {rms_db(nb):+.1f} dBFS")
    print(f"ERLE overall: {rms_db(ob) - rms_db(nb):+.1f} dB")

    # voice-active frames: 50ms grid, off-capture band energy 10dB+
    # above the off floor (speech gaps diluted the overall number)
    hop = SR // 20
    m = n // hop
    oe = (ob[:m * hop].reshape(m, hop) ** 2).mean(axis=1)
    ne = (nb[:m * hop].reshape(m, hop) ** 2).mean(axis=1)
    thr = np.mean(floor_o ** 2) * 10.0
    act = oe > thr
    if act.sum() > 5:
        erle = 10 * np.log10(oe[act].sum() / ne[act].sum())
        print(f"ERLE voice-active ({act.sum()}/{m} frames): {erle:+.1f} dB")

    # onset windows: the barge-in gap metric (the first reply's residual
    # used to pass nearly uncancelled for ~3s and false-trigger
    # interruptions)
    for t1 in (1.0, 3.0):
        w = act[:int(t1 * SR / hop)]
        if w.sum() > 2:
            v = 10 * np.log10(oe[:len(w)][w].sum() / ne[:len(w)][w].sum())
            print(f"ERLE onset-{t1:.0f}s ({w.sum()} frames): {v:+.1f} dB")


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    ap.add_argument("--volume", type=int, default=80)
    ap.add_argument("--seconds", type=float, default=14.0)
    ap.add_argument("--clip", required=True)
    args = ap.parse_args()

    pcm = load_clip(args.clip, args.seconds)
    blob = encode_clip(pcm)
    seconds = len(pcm) / SR

    b = BrilliantBle()
    await b.connect(name=args.name, data_response_handler=receive_data)
    b._user_print_response_handler = receive_print
    try:
        await b.send_break_signal()
        await asyncio.sleep(0.5)
        stamp = datetime.now().strftime("%H%M%S")

        await upload_clip(b, blob)

        print(f"pass 1: AEC off, {seconds:.1f}s voice at v{args.volume}...")
        off = await run_pass(b, False, args.volume, seconds)
        save_wav(f"voice_off_{stamp}.wav", off)

        print(f"pass 2: AEC on, {seconds:.1f}s voice at v{args.volume}...")
        on = await run_pass(b, True, args.volume, seconds)
        save_wav(f"voice_on_{stamp}.wav", on)

        await b.send_lua("frame.file.remove_all()")
        score(off, on, seconds)
        print(f"\nwavs: voice_off_{stamp}.wav / voice_on_{stamp}.wav")
    finally:
        await b.send_reset_signal()
        await b.disconnect()


asyncio.run(main())
