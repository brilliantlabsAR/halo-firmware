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
On-device AEC loopback test.

Plays a known 16 kHz mono clip through the Halo speaker (LC3 over the audio
characteristic, exactly like a duplex voice app) while streaming the LC3
microphone back over BLE - once with frame.microphone.aec(false) and once
with aec(true) - then decodes both captures and reports how much speaker
echo the on-device canceller removed (ERLE), plus residual cross-correlation
against the stimulus.

The `lc3` module is provided by the `lc3py` package (Google liblc3). The mic and speaker both run 16 kHz mono LC3 @32 kbps, matching the
CONFIG_HALO_AUDIO_AEC v1 operating point.

  uv run loopback_test.py --name "Halo AB"
  uv run loopback_test.py --name "Halo AB" --wav story.wav --volume 100
  uv run loopback_test.py --name "Halo AB" --seconds 8   # synthetic stimulus

Writes loopback_<aec_off|aec_on>.wav next to the script for listening.
"""

import argparse
import asyncio
import os
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
FRAMES_PER_PACKET = 10  # 400B packet per 100ms, as in test_speaker_lc3.py

rx = bytearray()


def receive_data(data_view):
    rx.extend(bytes(data_view))


def load_stimulus(args) -> np.ndarray:
    """16 kHz mono float in [-1, 1]."""
    if args.wav:
        with wave.open(args.wav, "rb") as w:
            assert w.getframerate() == SR and w.getnchannels() == 1, \
                "stimulus must be 16 kHz mono"
            x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
        x = x.astype(np.float64) / 32768.0
        if args.seconds and len(x) > args.seconds * SR:
            x = x[: args.seconds * SR]
        return x

    # synthetic: speech-band noise bursts (400ms on / 200ms off)
    rng = np.random.default_rng(1234)
    n = (args.seconds or 8) * SR
    x = rng.standard_normal(n)
    # crude speech-band shaping
    from numpy.fft import irfft, rfft, rfftfreq
    X = rfft(x)
    f = rfftfreq(n, 1 / SR)
    X *= np.clip(1.0 - np.abs(f - 900) / 2600, 0.05, 1.0)
    x = irfft(X, n)
    env = (((np.arange(n) // (SR * 6 // 10)) % 1) == 0).astype(float)
    env = np.where((np.arange(n) % (SR * 6 // 10)) < SR * 4 // 10, 1.0, 0.0)
    x = x * env
    return 0.5 * x / (np.abs(x).max() + 1e-9)


def encode_lc3(x: np.ndarray) -> bytes:
    enc = lc3.Encoder(FRAME_MS * 1000, SR, 1)
    pcm = (np.clip(x, -1, 1) * 32767).astype(np.int16)
    n_frames = len(pcm) // PCM_FRAME_SAMPLES
    out = bytearray()
    for i in range(n_frames):
        chunk = pcm[i * PCM_FRAME_SAMPLES: (i + 1) * PCM_FRAME_SAMPLES]
        out += enc.encode(chunk.tobytes(), LC3_FRAME_BYTES, bit_depth=16)
    return bytes(out)


def _decode_at(data: bytes, off: int) -> np.ndarray:
    dec = lc3.Decoder(FRAME_MS * 1000, SR, 1)
    out = []
    for i in range(off, len(data) - LC3_FRAME_BYTES + 1, LC3_FRAME_BYTES):
        out.append(np.frombuffer(
            dec.decode(data[i: i + LC3_FRAME_BYTES], bit_depth=16),
            dtype=np.int16))
    if not out:
        return np.zeros(0)
    return np.concatenate(out).astype(np.float64) / 32768.0


def decode_lc3(data: bytes) -> np.ndarray:
    """Decode the mic byte stream, auto-detecting the frame alignment.

    The stream can arrive with a few bytes of leading junk (observed: a
    2-byte prefix), and misaligned LC3 decodes to steady noise. Real audio
    is amplitude-modulated, so pick the offset whose decoded envelope has
    the highest modulation index.
    """
    best = (None, -1.0, 0)
    for off in range(LC3_FRAME_BYTES):
        x = _decode_at(data, off)
        if len(x) < SR:
            return x
        seg = np.abs(x[SR // 2:])  # skip start transient
        hop = SR // 100
        env = seg[: len(seg) // hop * hop].reshape(-1, hop).mean(axis=1)
        # ABSOLUTE envelope variation: real audio is both loud and
        # modulated. (A ratio like std/mean is a trap: a wrong offset
        # decoding to near-silence with rare blips has a tiny mean and
        # scores arbitrarily high.)
        score = float(env.std())
        if score > best[1]:
            best = (x, score, off)
    x, score, off = best
    if off:
        print(f"  (stream alignment: {off} leading bytes skipped, "
              f"env-std {score:.4f})")
    return x


def save_wav(path, x):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())


async def run_pass(b: BrilliantBle, lc3_clip: bytes, aec_on: bool,
                   volume: int, out_path: str):
    """One playback+capture pass; returns (mic_pcm, play_start_s, play_len_s)."""
    global rx
    rx = bytearray()

    await b.send_lua(f"frame.microphone.aec({'true' if aec_on else 'false'})")
    await b.send_lua(
        f"frame.speaker.start{{encoder='lc3', sample_rate={SR}, channels=1, "
        f"duration=1000, bitrate={BITRATE}, volume={volume}}}")

    # mic.start and the streaming read-loop MUST be one Lua command:
    # captures come back as all-zero LC3 when they are split across two
    # REPL commands (mechanism unclear; single-command is the proven
    # pattern). The REPL is blocked until break; the audio characteristic
    # bypasses the REPL so playback still works. The tiny sleep on empty
    # reads keeps the Lua VM from starving other threads.
    loop_started = asyncio.get_event_loop().time()
    asyncio.ensure_future(b.send_lua(
        f"frame.microphone.start{{encoder='lc3', sample_rate={SR}, "
        f"bitrate={BITRATE}, channels=1}} "
        "while true do "
        "s=frame.microphone.read(240) "
        "if s==nil then break end "
        "if s~='' then "
        "while true do if (pcall(frame.bluetooth.send,s)) then break end end "
        "else frame.sleep(0.002) "
        "end "
        "end"))
    await asyncio.sleep(1.0)  # lead-in silence in the capture

    # pace the clip to the speaker: FRAMES_PER_PACKET frames per write, at
    # 90% of real-time so host timer jitter and BLE contention can't
    # underrun the device buffer (the speaker session buffers ~1s)
    play_start = asyncio.get_event_loop().time() - loop_started
    packet = LC3_FRAME_BYTES * FRAMES_PER_PACKET
    for i in range(0, len(lc3_clip), packet):
        await b.send_audio(lc3_clip[i: i + packet], await_bt_response=False)
        await asyncio.sleep(FRAME_MS * FRAMES_PER_PACKET / 1000 * 0.90)
    play_len = len(lc3_clip) / LC3_FRAME_BYTES * FRAME_MS / 1000

    await asyncio.sleep(1.0)  # tail

    await b.send_break_signal()
    await asyncio.sleep(0.5)
    await b.send_lua("frame.microphone.stop()")
    await b.send_lua("frame.speaker.stop()")
    await asyncio.sleep(0.5)

    mic = decode_lc3(bytes(rx))
    save_wav(out_path, mic)
    print(f"  captured {len(mic)/SR:.1f}s -> {out_path}")
    return mic, play_start, play_len


def score(mic: np.ndarray, play_start: float, play_len: float,
          stim: np.ndarray, label: str):
    """Band-passed RMS in the playback window + peak xcorr vs the stimulus."""
    a = int((play_start + 0.5) * SR)
    b_ = int((play_start + play_len - 0.5) * SR)
    if b_ > len(mic):
        b_ = len(mic)
    seg = mic[a:b_]
    lead = mic[: int(0.8 * SR)]  # ambient reference from the lead-in

    # simple speech-band emphasis via FFT mask (avoid scipy dependency)
    def bp_rms(x):
        if len(x) < SR // 4:
            return 0.0
        X = np.fft.rfft(x)
        f = np.fft.rfftfreq(len(x), 1 / SR)
        X[(f < 300) | (f > 3400)] = 0
        y = np.fft.irfft(X, len(x))
        return float(np.sqrt(np.mean(y ** 2)))

    seg_rms = bp_rms(seg)
    amb_rms = bp_rms(lead)
    # normalized cross-correlation peak against the stimulus
    m = seg - seg.mean()
    s = stim[: len(m)] - stim[: len(m)].mean()
    if len(s) and len(m):
        c = np.correlate(m, s, mode="valid" if len(m) >= len(s) else "full")
        xc = float(np.abs(c).max() /
                   (np.linalg.norm(m) * np.linalg.norm(s) + 1e-12))
    else:
        xc = 0.0
    print(f"  {label}: playback-window RMS {20*np.log10(seg_rms+1e-9):+.1f} dBFS "
          f"(ambient {20*np.log10(amb_rms+1e-9):+.1f}), stim xcorr {xc:.3f}")
    return seg_rms


async def main(args):
    stim = load_stimulus(args)
    lc3_clip = encode_lc3(stim)
    print(f"stimulus: {len(stim)/SR:.1f}s, {len(lc3_clip)} LC3 bytes")

    b = BrilliantBle()
    await b.connect(data_response_handler=receive_data,
                    name=args.name) if args.name else \
        await b.connect(data_response_handler=receive_data)
    print("connected")

    stamp = datetime.now().strftime("%H%M%S")

    if args.speak_test:
        # mic-only sanity window: talk near the device while this runs
        global rx
        rx = bytearray()
        await b.send_lua("frame.microphone.aec(false)")
        print(f"SPEAK NOW - recording {args.speak_test}s of mic only...")
        # single command: see the note in run_pass
        asyncio.ensure_future(b.send_lua(
            f"frame.microphone.start{{encoder='lc3', sample_rate={SR}, "
            f"bitrate={BITRATE}, channels=1}} "
            "while true do s=frame.microphone.read(240) "
            "if s==nil then break end "
            "if s~='' then "
            "while true do if (pcall(frame.bluetooth.send,s)) then break end end "
            "else frame.sleep(0.002) end end"))
        await asyncio.sleep(args.speak_test)
        await b.send_break_signal()
        await asyncio.sleep(0.5)
        await b.send_lua("frame.microphone.stop()")
        mic = decode_lc3(bytes(rx))
        path = f"loopback_speaktest_{stamp}.wav"
        save_wav(path, mic)
        r = np.sqrt(np.mean(mic ** 2)) if len(mic) else 0
        print(f"  speak test: {len(mic)/SR:.1f}s captured, "
              f"rms {20*np.log10(r+1e-9):+.1f} dBFS -> {path}")
    off, off_t0, off_len = await run_pass(
        b, lc3_clip, aec_on=False, volume=args.volume,
        out_path=f"loopback_aec_off_{stamp}.wav")
    on, on_t0, on_len = await run_pass(
        b, lc3_clip, aec_on=True, volume=args.volume,
        out_path=f"loopback_aec_on_{stamp}.wav")

    await b.disconnect()

    print("\nresults:")
    rms_off = score(off, off_t0, off_len, stim, "AEC off")
    rms_on = score(on, on_t0, on_len, stim, "AEC on ")
    if rms_on > 0:
        print(f"  ERLE (band-passed, playback window): "
              f"{20*np.log10(rms_off/(rms_on+1e-12)):+.1f} dB")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default=None, help='device name, e.g. "Halo AB"')
    ap.add_argument("--wav", default=None,
                    help="16 kHz mono WAV stimulus (default: synthetic bursts)")
    ap.add_argument("--seconds", type=int, default=8,
                    help="stimulus length cap in seconds")
    ap.add_argument("--volume", type=int, default=100)
    ap.add_argument("--speak-test", type=int, default=0, metavar="SECONDS",
                    help="record N seconds of mic only first (talk near the "
                         "device) as a mic sanity check")
    asyncio.run(main(ap.parse_args()))
