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
AEC ERLE measurement with a gap-free, on-device stimulus.

The clock audit (see README) showed the PDM/I2S converters are crystal-
locked and the historical "drift" was BLE stimulus-feed starvation. This
test removes the transport entirely: the device synthesizes white noise in
Lua and feeds it to the speaker through frame.speaker.play's back-pressure
(so the emission is gap-free by construction), while the mic is captured
byte-exact to /lfs as LC3, once with AEC off and once with AEC on.

White noise is stationary and broadband, so ERLE is just the speech-band
RMS ratio between the two passes over the steady-state window (onset
reported separately to watch convergence).

  uv run erle_local_test.py --name "Halo AB" --volume 80 --seconds 15
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
BLOCK = 320  # 20ms mono frames per speaker feed
LEAD_S = 1.0   # mic-only lead-in (noise floor, PDM pop excluded)
TAIL_S = 1.0   # mic-only tail after playback stops

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
    """FFT brickwall band-pass; fine for RMS metrics."""
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1.0 / SR)
    X[(f < lo) | (f > hi)] = 0
    return np.fft.irfft(X, n=len(x))


def rms_db(x):
    return 20 * np.log10(np.sqrt(np.mean(x ** 2)) + 1e-12)


async def run_pass(b, aec_on, volume, seconds):
    global rx
    blocks = int(seconds * SR // BLOCK)
    target = int((LEAD_S + seconds + TAIL_S) * BITRATE // 8)

    await b.send_lua(f"frame.microphone.aec({'true' if aec_on else 'false'})")
    await b.send_lua("frame.file.remove_all()")
    await asyncio.sleep(0.3)

    # Capture mic LC3 to /lfs while feeding locally generated white noise
    # to the speaker. Noise blocks are pregenerated (16 blocks = 0.32s
    # cycle, still broadband) so the real-time loop only plays and drains;
    # speaker.play back-pressure paces it at the I2S rate - no BLE, no
    # gaps. Sent as sequential REPL chunks (payload limit) sharing device
    # globals; the mic ring covers the ~100ms gaps between chunks.
    printed.clear()
    await b.send_lua(
        "math.randomseed(1234) nz={} for i=1,16 do local t={} "
        "for j=1,320 do t[j]=string.pack('<i2',math.random(-6000,6000)) end "
        "nz[i]=table.concat(t) end print('ok1')",
        await_print=True, timeout=20)
    # Drain to a RAM table during the session - /lfs writes stall the Lua
    # thread long enough (flash erase) to drain the 160ms speaker DMA
    # queue, i.e. the capture itself was injecting emission gaps. The
    # whole capture (~70KB LC3) goes to /lfs only after playback ends.
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
        f"frame.speaker.start{{sample_rate={SR}, channels=1, volume={volume}}} "
        f"for i=1,{blocks} do frame.speaker.play(nz[(i % 16) + 1]) drainf() end "
        "frame.speaker.stop() print('ok4')",
        await_print=True, timeout=seconds + 60)
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
            "print(s.w_norm2..' '..s.p_ref..' '..s.p_err..' '..s.p_mic..' '"
            "..s.resyncs..' '..s.norm_clamps..' '..s.ref_underruns..' '..s.ref_pads..' '..s.feed_gaps..' '..s.feed_gap_ms)",
            await_print=True, timeout=10)
        print(f"  aec stats (w_norm2 p_ref p_err p_mic resyncs clamps underruns pads gaps gap_ms): "
              f"{printed[-1]}")

    # retrieve byte-exact (same proven loop as loopback_lfs.py)
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
    onset = int(1.5 * SR)
    for name, x in (("off", off), ("on", on)):
        xb = bandpass(x)
        # exclude the PDM start pop (first 0.5s) from the floor window
        floor = xb[int(0.5 * SR):lead]
        play = xb[lead:lead + int(seconds * SR)]
        print(f"\nAEC {name}: floor {rms_db(floor):+.1f} dBFS, "
              f"onset(1.5s) {rms_db(play[:onset]):+.1f} dBFS, "
              f"steady {rms_db(play[onset:]):+.1f} dBFS")
    ob = bandpass(off)[lead:lead + int(seconds * SR)]
    nb = bandpass(on)[lead:lead + int(seconds * SR)]
    print(f"\nERLE onset:  {rms_db(ob[:onset]) - rms_db(nb[:onset]):+.1f} dB")
    print(f"ERLE steady: {rms_db(ob[onset:]) - rms_db(nb[onset:]):+.1f} dB")


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    ap.add_argument("--volume", type=int, default=80)
    ap.add_argument("--seconds", type=int, default=15)
    args = ap.parse_args()

    b = BrilliantBle()
    await b.connect(name=args.name, data_response_handler=receive_data)
    b._user_print_response_handler = receive_print
    try:
        await b.send_break_signal()
        await asyncio.sleep(0.5)
        stamp = datetime.now().strftime("%H%M%S")

        print(f"pass 1: AEC off, {args.seconds}s noise at v{args.volume}...")
        off = await run_pass(b, False, args.volume, args.seconds)
        save_wav(f"erle_off_{stamp}.wav", off)

        print(f"pass 2: AEC on, {args.seconds}s noise at v{args.volume}...")
        on = await run_pass(b, True, args.volume, args.seconds)
        save_wav(f"erle_on_{stamp}.wav", on)

        await b.send_lua("frame.file.remove_all()")
        score(off, on, args.seconds)
        print(f"\nwavs: erle_off_{stamp}.wav / erle_on_{stamp}.wav")
    finally:
        await b.send_reset_signal()
        await b.disconnect()


asyncio.run(main())
