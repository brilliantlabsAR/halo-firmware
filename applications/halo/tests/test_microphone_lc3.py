#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = ["brilliant-ble>=3.1.1,<4", "numpy", "lc3py"]
# ///
"""
Record LC3-encoded audio from the Halo/Frame microphone over BLE, decode it,
and save both the raw .lc3 and a decoded .wav.

Run with: uv run test_microphone_lc3.py [options]   (deps auto-resolved; no venv)
The `lc3` module is provided by the `lc3py` package (Google liblc3).

This adds the LC3 codec on top of the DMIC/PDM front-end (but still not the
LE-Audio ISO transport). Compare against test_microphone.py (raw PCM) to tell
whether the codec config is degrading quality vs the front-end itself.

Defaults match LE-Audio voice (LC3 16_2): 16 kHz, 32 kbps, 10 ms frames, mono
(= 40 octets/frame). Firmware constraints: sample_rate in {8000, 16000};
channels in {1, 2}; gain in [-10, 10].

Examples:
  python test_microphone_lc3.py                              # 16 kHz mono 32 kbps
  python test_microphone_lc3.py --rate 8000 --bitrate 16000
  python test_microphone_lc3.py --channels 2 --seconds 10
  python test_microphone_lc3.py --out-wav take1.wav --no-lc3
  python test_microphone_lc3.py --name "Halo AB"
"""

import argparse
import asyncio
import wave
from datetime import datetime

import lc3
import numpy as np
from brilliant_ble import BrilliantBle

audio_buffer = b""  # Global buffer to store incoming LC3 audio data


def receive_data(data):
    """Bluetooth data callback: append each chunk to the global buffer."""
    global audio_buffer
    audio_buffer += data
    print(f"Received {len(audio_buffer)} bytes", end="\r")


# Lua: read fixed chunks off the mic ring buffer and forward over BLE until stopped.
_STREAM_LUA = (
    "while true do "
    "local s=frame.microphone.read(240); "
    "if s==nil then break end "
    "if s~='' then "
    "while true do if (pcall(frame.bluetooth.send,s)) then break end end "
    "end "
    "end"
)


def _start_cmd(rate, bitrate, channels, gain, frame_ms):
    # duration is microseconds/10 to match the Alif LC3 enum: 7.5 ms -> 750,
    # 10 ms -> 1000 (lua_microphone.c:830, lc3_api.h:31-34).
    duration = int(round(frame_ms * 100))
    return (
        f"frame.microphone.start{{encoder='lc3', sample_rate={rate}, "
        f"bitrate={bitrate}, channels={channels}, gain={gain}, "
        f"duration={duration}}}"
    )


def _default_stem(rate, bitrate, channels):
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    return f"mic_lc3_{rate}hz_{bitrate}bps_{channels}ch_{ts}"


async def record(b: BrilliantBle, rate, bitrate, channels, gain, seconds,
                 frame_ms):
    """Record LC3 for `seconds` and return the raw LC3 byte buffer."""
    global audio_buffer
    audio_buffer = b""

    # Ensure the mic is stopped before starting.
    await b.send_break_signal()
    await asyncio.sleep(1)
    await b.send_lua("frame.microphone.stop()")

    print(f"Recording LC3: {rate} Hz, {bitrate} bps, {channels} ch, gain {gain}, "
          f"{seconds:g} s")

    await b.send_lua(_start_cmd(rate, bitrate, channels, gain, frame_ms))
    await asyncio.sleep(1)
    await b.send_lua(_STREAM_LUA)

    await asyncio.sleep(seconds)

    # Stop and let the tail of the buffer drain over BLE.
    await b.send_break_signal()
    await asyncio.sleep(1)
    await b.send_lua("frame.microphone.stop()")
    print("\nStopping recording...")
    await asyncio.sleep(5)

    return audio_buffer


def decode_to_wav(buf, rate, bitrate, frame_ms, channels, wav_path):
    """Decode the LC3 byte buffer to PCM and write a WAV."""
    # Bytes per on-air frame across all channels.
    frame_bytes = int(round(bitrate * frame_ms / 1000 / 8)) * channels
    # lc3.Decoder wants an int frame duration in microseconds.
    decoder = lc3.Decoder(int(round(frame_ms * 1000)), rate, 1)

    print(f"Decoding {len(buf)} bytes into {frame_bytes}-byte frames...")
    with wave.open(wav_path, "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(2)
        wf.setframerate(rate)

        for i in range(0, len(buf), frame_bytes):
            chunk = buf[i:i + frame_bytes]
            if len(chunk) < frame_bytes:
                break  # skip incomplete trailing frame
            try:
                if channels == 2:
                    half = frame_bytes // 2
                    pcm_l = decoder.decode(chunk[:half], bit_depth=16)
                    pcm_r = decoder.decode(chunk[half:], bit_depth=16)
                    interleaved = np.zeros(
                        len(np.frombuffer(pcm_l, dtype=np.int16)) * 2,
                        dtype=np.int16)
                    interleaved[0::2] = np.frombuffer(pcm_l, dtype=np.int16)
                    interleaved[1::2] = np.frombuffer(pcm_r, dtype=np.int16)
                    wf.writeframesraw(interleaved.tobytes())
                else:
                    wf.writeframesraw(decoder.decode(chunk, bit_depth=16))
            except Exception as e:
                print(f"\nDecoding error at offset {i}: {e}")
                break
    print(f"Saved decoded audio to {wav_path}")


def parse_args():
    p = argparse.ArgumentParser(
        description="Record LC3 off the Halo mic over BLE; save .lc3 + decoded .wav.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument(
        "--name",
        default=None,
        help='exact BLE device name, e.g. "Halo AB" or "Frame 4F"; '
             "defaults to the nearest device",
    )
    p.add_argument("--rate", type=int, default=16000, choices=[8000, 16000],
                   help="Sample rate (Hz)")
    p.add_argument("--bitrate", type=int, default=32000,
                   help="LC3 bitrate in bps (16 kHz voice = 32000 -> LC3 16_2)")
    p.add_argument("--frame-ms", type=float, default=10.0, choices=[7.5, 10.0],
                   help="LC3 frame duration in ms (sent to the device and "
                        "used for host-side decode)")
    p.add_argument("--channels", type=int, default=1, choices=[1, 2],
                   help="Channel count (mono/stereo)")
    p.add_argument("--gain", type=int, default=0, help="Mic gain, -10..10")
    p.add_argument("--seconds", type=float, default=5.0,
                   help="Recording duration in seconds")
    p.add_argument("--out-wav", default=None, help="Decoded WAV output path")
    p.add_argument("--out-lc3", default=None, help="Raw LC3 output path")
    p.add_argument("--no-lc3", action="store_true",
                   help="Do not write the raw .lc3 file")
    args = p.parse_args()
    if not -10 <= args.gain <= 10:
        p.error("--gain must be between -10 and 10")
    # firmware: "Bitrate must be multiple of 8000 and <= 96000"
    if args.bitrate % 8000 or not 0 < args.bitrate <= 96000:
        p.error("--bitrate must be a multiple of 8000 and <= 96000")
    return args


async def main(args):
    b = BrilliantBle()
    name = await b.connect(name=args.name, data_response_handler=receive_data)
    fw = await b.send_lua("print(frame.FIRMWARE_VERSION)", await_print=True)
    tag = await b.send_lua("print(frame.GIT_TAG)", await_print=True)
    batt = await b.send_lua("print(frame.battery_level())", await_print=True)
    print(f"{name} | firmware {fw} | git {tag} | battery {batt}%")

    buf = await record(
        b, args.rate, args.bitrate, args.channels, args.gain, args.seconds,
        args.frame_ms)
    await b.disconnect()

    if not buf:
        print("No audio received - nothing to save.")
        return

    stem = _default_stem(args.rate, args.bitrate, args.channels)
    if not args.no_lc3:
        lc3_path = args.out_lc3 or f"{stem}.lc3"
        with open(lc3_path, "wb") as f:
            f.write(buf)
        print(f"Saved raw LC3 to {lc3_path}  ({len(buf)} bytes)")

    wav_path = args.out_wav or f"{stem}.wav"
    decode_to_wav(buf, args.rate, args.bitrate, args.frame_ms,
                  args.channels, wav_path)


if __name__ == "__main__":
    asyncio.run(main(parse_args()))
