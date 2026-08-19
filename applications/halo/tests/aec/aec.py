#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = [
#     "numpy",
#     "scipy",
# ]
# ///
"""
aec — offline acoustic-echo-cancellation prototyping for the Halo.

The Halo's bone-conduction speaker and its mics live on the same frame a few
centimetres apart, so whatever the speaker plays leaks straight back into the
mic ("echo"). For a voice-assistant loop (see the flutter realtime_gemini
example) that echo is re-sent to the assistant as if the user had spoken —
barge-in is unusable without cancelling it. This harness works out the
*optimal subtraction* offline, on aligned (mic, far-end reference) WAV pairs,
before anything is committed to firmware.

Pipeline implemented here (each stage scored separately):

  1. bulk-delay estimation  — GCC-PHAT, both global and windowed (drift check).
     In firmware the delay is a couple of 20 ms blocks and near-constant; in
     phone-loop captures it is hundreds of ms and may drift if capture gaps
     were mispadded. Windowed tracking tells us which situation we're in.
  2. linear cancellation    — time-domain NLMS FIR adapted on the delayed
     reference, with a Geigel-style double-talk detector freezing adaptation
     while the wearer speaks. This is the part that ports directly to C
     (block NLMS on int16, no FFT needed at 16 kHz with a few hundred taps).
  3. residual suppression   — optional STFT-domain over-subtraction gated by
     the *predicted* echo magnitude (the NLMS filter output). This is what
     mops up the nonlinear residual that no linear FIR can reach: the speaker
     protection limiter (spk_protect.c) and the transducer itself distort, and
     phone-side measurements showed the mic picking up ~+12 dB more HF (vs LF)
     than the reference contains during loud playback.

Honest limitations, stated up front:
  - A linear FIR models the *linear* part of speaker→mic coupling only. The
    measured harmonic distortion means stage 3 (or a nonlinear model) is
    expected to matter on this hardware. Judge results by listening AND by
    ERLE — ERLE alone under-sells suppression that removes structured speech
    (the thing that trips the assistant's VAD) from under broadband noise.
  - Mic (T5838 PDM) and speaker (MAX98357A I2S) are clocked independently:
    long adaptive runs must tolerate slow drift. NLMS re-converges on its own;
    a production filter should also re-anchor the bulk delay occasionally.
  - ERLE is measured on ref-active, near-end-quiet frames in the speech band;
    with double-talk-heavy captures the number is only indicative.

Usage
  # full pipeline on a capture pair from the realtime_gemini app
  uv run aec.py aec_mic.wav aec_speaker_ref.wav

  # linear stage only, custom filter length (taps at 16 kHz; 640 = 40 ms)
  uv run aec.py mic.wav ref.wav --taps 640 --no-suppress

  # skip delay search when the pair is already aligned (e.g. firmware taps)
  uv run aec.py mic.wav ref.wav --delay 0

Writes <mic>_aec.wav (linear) and <mic>_aec_sup.wav (with suppression).
"""
import argparse
import os
import sys
import wave

import numpy as np
from scipy import signal

SR = 16000


# ---------------------------------------------------------------- I/O

def read_wav(path):
    with wave.open(path, "rb") as w:
        if w.getframerate() != SR or w.getnchannels() != 1 or w.getsampwidth() != 2:
            sys.exit(f"{path}: expected {SR} Hz mono 16-bit PCM")
        data = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
    return data.astype(np.float64) / 32768.0


def write_wav(path, x):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())


# ------------------------------------------------------- delay estimation

def gcc_phat(a, b, max_lag):
    """Lag of a relative to b (positive: a lags b), by phase transform."""
    nfft = 1 << int(np.ceil(np.log2(len(a) + len(b))))
    A = np.fft.rfft(a, nfft)
    B = np.fft.rfft(b, nfft)
    R = A * np.conj(B)
    R /= np.abs(R) + 1e-12
    cc = np.fft.irfft(R, nfft)
    cc = np.concatenate((cc[-max_lag:], cc[: max_lag + 1]))
    k = int(np.argmax(np.abs(cc)))
    sharp = float(np.abs(cc[k]) / (np.abs(cc).mean() + 1e-12))
    return k - max_lag, sharp


def estimate_delay(mic, ref, max_lag=SR):
    """Global bulk delay + windowed drift check over ref-active regions."""
    d, sharp = gcc_phat(mic, ref, max_lag)
    print(f"global bulk delay: {d} samples ({d/SR*1000:+.1f} ms), "
          f"peak sharpness {sharp:.0f}")

    win = 2 * SR
    drifts = []
    for a in range(0, len(ref) - win, win):
        r = ref[a : a + win]
        if float(np.sqrt(np.mean(r**2))) < 0.01:
            continue
        b_end = min(a + win + max_lag, len(mic))
        dd, ss = gcc_phat(mic[a:b_end], np.concatenate([r, np.zeros(b_end - a - win)]),
                          max_lag)
        if ss > 8:
            drifts.append((a / SR, dd))
    if drifts:
        lo = min(x[1] for x in drifts)
        hi = max(x[1] for x in drifts)
        print(f"windowed delays: {len(drifts)} windows, "
              f"min {lo/SR*1000:+.0f} ms max {hi/SR*1000:+.0f} ms "
              f"({'stable' if hi-lo < SR//100 else 'DRIFTING — expect reduced ERLE'})")
    return d


# --------------------------------------------------------------- NLMS

def nlms(mic, ref, taps, mu=0.5, dtd_threshold=2.0):
    """Time-domain NLMS echo canceller with a Geigel-ish double-talk freeze.

    mic and ref must already be bulk-delay aligned (ref pre-delayed).
    Returns (residual, echo_estimate, adapt_fraction).
    """
    n = min(len(mic), len(ref))
    w = np.zeros(taps)
    xbuf = np.zeros(taps)
    res = np.zeros(n)
    est = np.zeros(n)
    eps = 1e-6

    # short-term powers for the double-talk detector
    p_ref = 0.0
    p_err = 0.0
    alpha = 1.0 / (0.05 * SR)  # ~50 ms
    adapted = 0

    for i in range(n):
        xbuf[1:] = xbuf[:-1]
        xbuf[0] = ref[i]
        y = float(w @ xbuf)
        e = mic[i] - y
        est[i] = y
        res[i] = e

        p_ref += alpha * (ref[i] * ref[i] - p_ref)
        p_err += alpha * (e * e - p_err)

        # freeze adaptation when the error is large relative to what the
        # echo path could produce -> near-end speech present
        if p_ref > 1e-8 and p_err < dtd_threshold * p_ref:
            w += mu * e * xbuf / (float(xbuf @ xbuf) + eps)
            adapted += 1
            # divergence guard: an echo path can't have gain >> 1; a norm
            # blowing past that means we're adapting on misaligned data
            norm = float(w @ w)
            if norm > 4.0:
                w *= 2.0 / np.sqrt(norm)

    return res, est, adapted / max(n, 1)


# ------------------------------------------------- residual suppression

def suppress_residual(res, est, over=2.0, floor=0.1, nfft=256, hop=128):
    """STFT over-subtraction: attenuate residual bins where the *predicted*
    echo (est) still has energy. Gated by est, so near-end-only regions
    (est ~ 0) pass through untouched."""
    f, t, R = signal.stft(res, fs=SR, nperseg=nfft, noverlap=nfft - hop)
    _, _, E = signal.stft(est, fs=SR, nperseg=nfft, noverlap=nfft - hop)
    gain = np.maximum(1.0 - over * np.abs(E) / (np.abs(R) + 1e-9), floor)
    _, out = signal.istft(R * gain, fs=SR, nperseg=nfft, noverlap=nfft - hop)
    return out[: len(res)]


# --------------------------------------------------------------- scoring

def erle_db(mic, res, ref, label):
    """Speech-band ERLE over ref-active frames."""
    n = min(len(mic), len(res), len(ref))
    sos = signal.butter(4, [300, 3400], "bandpass", fs=SR, output="sos")
    m = signal.sosfilt(sos, mic[:n])
    r = signal.sosfilt(sos, res[:n])
    hot = np.abs(ref[:n]) > 0.01
    if hot.sum() < SR:
        print(f"{label}: not enough ref-active audio to score")
        return
    v = 10 * np.log10(float((m[hot] ** 2).mean()) /
                      (float((r[hot] ** 2).mean()) + 1e-15))
    print(f"{label}: ERLE {v:+.1f} dB over {hot.sum()/SR:.1f}s of playback")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("mic")
    ap.add_argument("ref")
    ap.add_argument("--taps", type=int, default=640,
                    help="FIR length in samples (640 = 40 ms @16k)")
    ap.add_argument("--mu", type=float, default=0.5, help="NLMS step size")
    ap.add_argument("--delay", type=int, default=None,
                    help="bulk delay in samples (default: estimate)")
    ap.add_argument("--no-suppress", action="store_true",
                    help="skip the residual-suppression stage")
    ap.add_argument("--segment", metavar="START:END",
                    help="analyze only this time range in seconds, e.g. 59:72 "
                         "(useful when the capture's delay drifts)")
    args = ap.parse_args()

    mic = read_wav(args.mic)
    ref = read_wav(args.ref)
    n = min(len(mic), len(ref))
    mic, ref = mic[:n], ref[:n]
    if args.segment:
        s0, s1 = (float(x) for x in args.segment.split(":"))
        mic = mic[int(s0 * SR) : int(s1 * SR)]
        ref = ref[int(s0 * SR) : int(s1 * SR)]
        n = len(mic)
    print(f"loaded {n/SR:.1f}s")

    d = args.delay if args.delay is not None else estimate_delay(mic, ref)
    if d > 0:
        ref_al = np.concatenate([np.zeros(d), ref])[:n]
    elif d < 0:
        ref_al = np.concatenate([ref[-d:], np.zeros(-d)])
    else:
        ref_al = ref

    res, est, frac = nlms(mic, ref_al, args.taps, args.mu)
    print(f"NLMS {args.taps} taps ({args.taps/SR*1000:.0f} ms), "
          f"adapting {frac*100:.0f}% of the time")
    erle_db(mic, res, ref_al, "linear NLMS")

    base = os.path.splitext(args.mic)[0]
    write_wav(f"{base}_aec.wav", res)
    print(f"wrote {base}_aec.wav")

    if not args.no_suppress:
        sup = suppress_residual(res, est)
        erle_db(mic, sup, ref_al, "NLMS + residual suppression")
        write_wav(f"{base}_aec_sup.wav", sup)
        print(f"wrote {base}_aec_sup.wav")


if __name__ == "__main__":
    main()
