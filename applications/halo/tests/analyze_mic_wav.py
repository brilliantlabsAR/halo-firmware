#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = [
#     "numpy",
# ]
# ///
"""
Split a (stereo) mic WAV into per-channel mono WAVs and print diagnostics.

Built to localize the stereo LE-Audio buzzing on the Halo (two edge-multiplexed
T5838 PDM mics on a shared DATA/CLK). Run it on a stereo capture from
test_microphone.py --channels 2. It tells you whether the second channel is:
  - dead / noise      -> Left mic not clocked/enabled (very low or flat-noise RMS)
  - a shifted copy    -> demux/edge-timing bug (high cross-correlation at a lag)
  - a real 2nd signal -> genuine stereo (high-ish corr at lag 0, similar RMS)

Prints per-channel RMS/peak/DC and the L/R cross-correlation (best lag), and
writes <stem>_ch0.wav / <stem>_ch1.wav so you can listen to each in isolation.

  uv run analyze_mic_wav.py mic_pcm_16000hz_2ch_16bit_<ts>.wav
"""

import argparse
import wave

import numpy as np


def load_wav(path):
    with wave.open(path, "rb") as wf:
        ch = wf.getnchannels()
        sw = wf.getsampwidth()
        rate = wf.getframerate()
        raw = wf.readframes(wf.getnframes())
    dtype = {1: np.uint8, 2: np.int16}.get(sw)
    if dtype is None:
        raise SystemExit(f"Unsupported sample width: {sw} bytes")
    data = np.frombuffer(raw, dtype=dtype).astype(np.float64)
    if sw == 1:  # 8-bit WAV is unsigned; center it
        data -= 128.0
    if ch > 1:
        data = data.reshape(-1, ch)
    else:
        data = data.reshape(-1, 1)
    return data, rate, sw


def chan_stats(x):
    rms = float(np.sqrt(np.mean(x ** 2)))
    peak = float(np.max(np.abs(x))) if x.size else 0.0
    dc = float(np.mean(x))
    return rms, peak, dc


def best_lag_correlation(a, b, max_lag=64):
    """Normalized cross-correlation of a vs b over +/-max_lag samples."""
    a = a - a.mean()
    b = b - b.mean()
    denom = np.sqrt(np.sum(a ** 2) * np.sum(b ** 2))
    if denom == 0:
        return 0.0, 0
    n = len(a)
    best_r, best_lag = 0.0, 0
    for lag in range(-max_lag, max_lag + 1):
        if lag < 0:
            r = np.dot(a[:n + lag], b[-lag:]) / denom
        elif lag > 0:
            r = np.dot(a[lag:], b[:n - lag]) / denom
        else:
            r = np.dot(a, b) / denom
        if abs(r) > abs(best_r):
            best_r, best_lag = float(r), lag
    return best_r, best_lag


def write_channel(path, samples_int16, rate):
    with wave.open(path, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(rate)
        wf.writeframes(samples_int16.astype(np.int16).tobytes())


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("wav", help="Input WAV (mono or stereo)")
    p.add_argument("--max-lag", type=int, default=64,
                   help="Max sample lag to search for L/R correlation")
    args = p.parse_args()

    data, rate, sw = load_wav(args.wav)
    nframes, nch = data.shape
    print(f"{args.wav}: {nch} ch, {rate} Hz, {sw*8}-bit, "
          f"{nframes} frames ({nframes/rate:.2f} s)\n")

    full = np.iinfo(np.int16).max
    for c in range(nch):
        rms, peak, dc = chan_stats(data[:, c])
        print(f"  ch{c}: RMS {rms:8.1f} ({20*np.log10(max(rms,1e-9)/full):6.1f} dBFS)  "
              f"peak {peak:8.1f}  DC {dc:8.1f}")

    if nch >= 2:
        r, lag = best_lag_correlation(data[:, 0], data[:, 1], args.max_lag)
        print(f"\n  L/R correlation: best r={r:+.3f} at lag {lag} samples "
              f"({lag/rate*1e6:+.0f} us)")
        r0, _ = best_lag_correlation(data[:, 0], data[:, 1], 0)
        print(f"  L/R correlation at lag 0: r={r0:+.3f}")
        print("  Hints: |r|~1 at lag 0 -> duplicate; |r|~1 at lag!=0 -> shifted "
              "(demux/edge timing); r~0 with low ch1 RMS -> dead/noise channel.")

        stem = args.wav.rsplit(".", 1)[0]
        for c in range(nch):
            out = f"{stem}_ch{c}.wav"
            write_channel(out, data[:, c], rate)
            print(f"  wrote {out}")


if __name__ == "__main__":
    main()
