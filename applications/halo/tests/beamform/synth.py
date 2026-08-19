#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = [
#     "numpy",
#     "scipy",
# ]
# ///
"""
synth — build a synthetic two-mic scene for the Halo geometry so the beamformer
prototypes can be validated WITHOUT any device recordings.

It places a "voice" target at broadside (theta=0, zero inter-mic delay), an
interferer at an arbitrary angle (nonzero delay), and an isotropic diffuse-noise
field with the physically-correct sinc(2 f d / c) inter-mic coherence. It writes:

  <out>.wav             the stereo mic pair (what the device would capture)
  <out>_target.wav      the clean broadside target alone (ground truth for SNR)

so beamform.py can score any method against the known-clean target.

Examples
  uv run synth.py --out scene.wav                       # defaults: d=10cm, interferer at 60deg
  uv run synth.py --spacing-cm 13 --interferer-deg 75 --isnr 0 --dsnr 5
  uv run synth.py --source ../mic_pcm_16000hz_1ch_16bit_20260708_160127.wav  # real voice as target
"""
import argparse

import numpy as np
from scipy import signal

import dualmic as dm

C = dm.C_SOUND


def voice_like(n, fs, seed=0):
    """A voice-ish test signal: a glottal pulse train (rising pitch) shaped by a
    couple of formants and gated by a ~4 Hz syllabic envelope. Non-stationary and
    voice-band, which is what the spatial/coherence math needs to be exercised."""
    rng = np.random.default_rng(seed)
    t = np.arange(n) / fs
    f0 = 110 + 25 * np.sin(2 * np.pi * 0.3 * t)  # slow pitch wobble
    phase = 2 * np.pi * np.cumsum(f0) / fs
    src = np.zeros(n)
    for h in range(1, 25):  # harmonics with 1/h rolloff
        src += (1.0 / h) * np.sin(h * phase)
    # formants
    for fc, bw in [(600, 90), (1200, 110), (2600, 160)]:
        b, a = signal.iirpeak(fc / (fs / 2), fc / bw)
        src = signal.lfilter(b, a, src)
    # syllabic gating (voiced/unvoiced bursts)
    env = np.clip(0.5 + 0.9 * np.sin(2 * np.pi * 3.5 * t + rng.uniform(0, 6)), 0, 1)
    env *= (signal.lfilter(*signal.butter(2, 6, fs=fs), rng.random(n) > 0.15))
    src *= env
    return (src / (np.max(np.abs(src)) + 1e-9)).astype(np.float32)


def diffuse_pair(n, fs, d_cm, n_waves=48, seed=1):
    """Isotropic diffuse noise on the two mics: sum of many plane waves from
    angles spread over the sphere, each delayed by d*sin(theta)/c. Reproduces the
    real sinc() inter-mic coherence rather than faking it."""
    rng = np.random.default_rng(seed)
    d = d_cm / 100.0
    left = np.zeros(n)
    right = np.zeros(n)
    for _ in range(n_waves):
        w = rng.standard_normal(n)
        # uniform on sphere -> cos(theta) uniform in [-1,1]; project onto mic axis
        sin_t = rng.uniform(-1, 1)
        tau = d * sin_t / C * fs  # samples
        left += dm.frac_delay(w, +tau / 2)
        right += dm.frac_delay(w, -tau / 2)
    s = np.sqrt(n_waves)
    return (left / s).astype(np.float32), (right / s).astype(np.float32)


def place(mono, angle_deg, d_cm, fs):
    """Put a source at angle_deg (0=broadside) onto the two mics with the correct
    fractional inter-mic delay. Positive angle -> arrives at L first."""
    d = d_cm / 100.0
    tau = d * np.sin(np.deg2rad(angle_deg)) / C * fs
    return dm.frac_delay(mono, +tau / 2), dm.frac_delay(mono, -tau / 2)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="scene.wav")
    ap.add_argument("--seconds", type=float, default=6.0)
    ap.add_argument("--fs", type=int, default=16000)
    ap.add_argument("--spacing-cm", type=float, default=10.0)
    ap.add_argument("--source", help="mono WAV to use as the broadside target "
                    "(default: synthetic voice-like signal)")
    ap.add_argument("--interferer-deg", type=float, default=60.0,
                    help="angle of a competing talker (0=broadside/off)")
    ap.add_argument("--isnr", type=float, default=3.0,
                    help="target-to-interferer ratio in dB")
    ap.add_argument("--dsnr", type=float, default=6.0,
                    help="target-to-diffuse-noise ratio in dB")
    ap.add_argument("--mic-gain-db", type=float, default=0.0,
                    help="inject an L/R gain mismatch to test calibration")
    ap.add_argument("--mic-delay-samp", type=float, default=0.0,
                    help="inject an L/R phase mismatch (samples) to test calibration")
    args = ap.parse_args()

    fs = args.fs
    n = int(args.seconds * fs)

    if args.source:
        tgt, sfs = dm.read_mono(args.source)
        if sfs != fs:
            tgt = signal.resample_poly(tgt, fs, sfs)
        tgt = np.resize(tgt, n)
        tgt = tgt / (np.max(np.abs(tgt)) + 1e-9)
    else:
        tgt = voice_like(n, fs, seed=0)

    # broadside target (zero delay) -> identical on both mics
    tL, tR = place(tgt, 0.0, args.spacing_cm, fs)

    # interferer: a second voice-like signal off to the side
    intr = voice_like(n, fs, seed=7)
    iL, iR = place(intr, args.interferer_deg, args.spacing_cm, fs)
    g_i = 10 ** (-args.isnr / 20.0)

    # diffuse field
    dL, dR = diffuse_pair(n, fs, args.spacing_cm)
    g_d = 10 ** (-args.dsnr / 20.0)

    L = tL + g_i * iL + g_d * dL
    R = tR + g_i * iR + g_d * dR

    # optional mic mismatch, applied to R only
    if args.mic_delay_samp:
        R = dm.frac_delay(R, args.mic_delay_samp)
    if args.mic_gain_db:
        R = R * 10 ** (args.mic_gain_db / 20.0)

    peak = max(np.max(np.abs(L)), np.max(np.abs(R))) + 1e-9
    L, R = 0.9 * L / peak, 0.9 * R / peak
    tgt_ref = 0.9 * (tL) / peak  # ground-truth target as heard at mic L, same scaling

    dm.write_stereo(args.out, L, R, fs)
    ref_path = args.out.replace(".wav", "_target.wav")
    dm.write_mono(ref_path, tgt_ref, fs)

    print(f"wrote {args.out}  ({args.seconds:.1f}s, fs={fs}, d={args.spacing_cm}cm)")
    print(f"  target=broadside  interferer={args.interferer_deg:g}deg @ {args.isnr:g}dB"
          f"  diffuse @ {args.dsnr:g}dB")
    print(f"  f_alias = c/2d = {C / (2 * args.spacing_cm / 100):.0f} Hz")
    print(f"wrote {ref_path}  (clean broadside target, ground truth for SNR)")


if __name__ == "__main__":
    main()
