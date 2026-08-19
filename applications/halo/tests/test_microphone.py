#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = ["brilliant-ble>=3.1.1,<4", "numpy"]
# ///
"""
Record raw PCM from the Halo/Frame microphone over BLE and save a WAV.

Run with: uv run test_microphone.py [options]   (deps auto-resolved; no venv)

Captures straight off the DMIC/PDM front-end (pre-LC3), so it isolates
*front-end* audio quality from the LC3 codec and the LE-Audio ISO transport.
For an apples-to-apples comparison with LE-Audio voice, record at the defaults
(16 kHz / 16-bit / mono) and compare against test_microphone_lc3.py:

  - raw PCM sounds bad          -> front-end (DMIC filters/gain), codec exonerated
  - PCM clean but LC3 bad       -> codec config
  - both clean but LE-Audio bad -> ISO datapath

Useful for mic-quality localization experiments.

Firmware constraints (modules/halo/src/lua_microphone.c):
  sample_rate in {8000, 16000}; bit_depth 16 only; channels in {1, 2};
  gain in [-10, 10].

Examples:
  python test_microphone.py                                # 16 kHz mono 16-bit, 5 s
  python test_microphone.py --rate 8000 --bits 8 --channels 2
  python test_microphone.py --gain 4 --seconds 10 --out take1.wav
  python test_microphone.py --latency                      # also report startup latency
  python test_microphone.py --name "Halo AB"
"""

import argparse
import asyncio
import time
import wave
from datetime import datetime

import numpy as np
from brilliant_ble import BrilliantBle

audio_buffer = b""  # Global buffer to store incoming audio data


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


def _start_cmd(rate, bits, channels, gain):
    return (
        f"frame.microphone.start{{sample_rate={rate}, bit_depth={bits}, "
        f"channels={channels}, gain={gain}}}"
    )


def _default_out(rate, channels, bits):
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    return f"mic_pcm_{rate}hz_{channels}ch_{bits}bit_{ts}.wav"


async def measure_startup_latency(b: BrilliantBle, rate, bits, channels, gain,
                                  budget_ms):
    """Report wall-clock from microphone.start() to first BLE audio chunk.

    Relevant to the AAD wake-from-standby path (must not miss the wake word).
    Report-only: warns if over budget rather than failing the capture.
    Optional UART logs: DMIC block, ring put, Lua read. BLE wall-clock is
    sufficient without UART.
    """
    global audio_buffer
    audio_buffer = b""

    await b.send_break_signal()
    await asyncio.sleep(0.5)
    await b.send_lua("frame.microphone.stop()")
    await asyncio.sleep(0.5)

    t_start = time.perf_counter()
    await b.send_lua(_start_cmd(rate, bits, channels, gain))

    read_task = asyncio.create_task(b.send_lua(_STREAM_LUA))

    deadline = time.perf_counter() + 2.0
    while len(audio_buffer) == 0 and time.perf_counter() < deadline:
        await asyncio.sleep(0.005)

    latency_ms = (time.perf_counter() - t_start) * 1000

    try:
        await asyncio.wait_for(read_task, timeout=5.0)
    except asyncio.TimeoutError:
        pass

    # The _STREAM_LUA loop is still executing; the REPL runs one line at a
    # time, so stop() would sit in the buffer forever. Break out first.
    await b.send_break_signal()
    await asyncio.sleep(0.5)
    await b.send_lua("frame.microphone.stop()")

    if len(audio_buffer) == 0:
        print("\n=== Mic startup latency: NO DATA within 2 s ===")
        return None

    over = " (OVER BUDGET)" if latency_ms > budget_ms else ""
    print(f"\n=== Mic startup latency: {latency_ms:.1f} ms "
          f"(first BLE data, budget {budget_ms:.0f} ms){over} ===")
    return latency_ms


async def record_and_save(b: BrilliantBle, rate, bits, channels, gain, seconds,
                          out):
    """Record raw PCM for `seconds` and save a WAV to `out`."""
    global audio_buffer
    audio_buffer = b""

    # Ensure the mic is stopped before starting.
    await b.send_break_signal()
    await asyncio.sleep(1)
    await b.send_lua("frame.microphone.stop()")

    print(f"Recording PCM: {rate} Hz, {bits}-bit, {channels} ch, gain {gain}, "
          f"{seconds:g} s")

    await b.send_lua(_start_cmd(rate, bits, channels, gain))
    await b.send_lua(_STREAM_LUA)

    await asyncio.sleep(seconds)

    # Stop and let the tail of the buffer drain over BLE.
    await b.send_break_signal()
    await asyncio.sleep(1)
    await b.send_lua("frame.microphone.stop()")
    print("\nStopping recording...")
    await asyncio.sleep(5)

    if len(audio_buffer) == 0:
        print("No audio received - nothing to save.")
        return

    print("Converting to WAV...")
    if bits == 16:
        sampwidth = 2
        audio_data = np.frombuffer(audio_buffer, dtype=np.int16)
    else:  # 8-bit: device sends signed int8; WAV 8-bit is unsigned.
        sampwidth = 1
        signed = np.frombuffer(audio_buffer, dtype=np.int8)
        audio_data = (signed.astype(np.int16) + 128).astype(np.uint8)

    with wave.open(out, "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(sampwidth)
        wf.setframerate(rate)
        wf.writeframes(audio_data.tobytes())

    print(f"Audio saved as {out}  ({len(audio_buffer)} bytes)")


def parse_args():
    p = argparse.ArgumentParser(
        description="Record raw PCM off the Halo mic over BLE and save a WAV.",
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
    # 8-bit works again (#253): the firmware always captures and processes at
    # 16-bit and downconverts to signed 8-bit output after the AEC/DC-block
    # stages, so this is real audio, not the corrupted stream it used to be.
    p.add_argument("--bits", type=int, default=16, choices=[8, 16],
                   help="Bit depth (output format; capture runs at 16)")
    p.add_argument("--channels", type=int, default=1, choices=[1, 2],
                   help="Channel count (mono/stereo)")
    p.add_argument("--gain", type=int, default=0, help="Mic gain, -10..10")
    p.add_argument("--seconds", type=float, default=5.0,
                   help="Recording duration in seconds")
    p.add_argument("--out", default=None,
                   help="Output WAV path (default: mic_pcm_<rate>_<ch>_<bits>_<ts>.wav)")
    p.add_argument("--latency", action="store_true",
                   help="Also measure/report mic startup latency first")
    p.add_argument("--latency-budget-ms", type=float, default=200.0,
                   help="Latency budget for the report")
    args = p.parse_args()
    if not -10 <= args.gain <= 10:
        p.error("--gain must be between -10 and 10")
    return args


async def main(args):
    b = BrilliantBle()
    name = await b.connect(name=args.name, data_response_handler=receive_data)
    fw = await b.send_lua("print(frame.FIRMWARE_VERSION)", await_print=True)
    tag = await b.send_lua("print(frame.GIT_TAG)", await_print=True)
    batt = await b.send_lua("print(frame.battery_level())", await_print=True)
    print(f"{name} | firmware {fw} | git {tag} | battery {batt}%")

    if args.latency:
        await measure_startup_latency(
            b, args.rate, args.bits, args.channels, args.gain,
            args.latency_budget_ms)
    out = args.out or _default_out(args.rate, args.channels, args.bits)
    await record_and_save(
        b, args.rate, args.bits, args.channels, args.gain, args.seconds, out)
    await b.disconnect()


if __name__ == "__main__":
    asyncio.run(main(parse_args()))
