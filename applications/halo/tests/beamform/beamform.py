#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = [
#     "numpy",
#     "scipy",
# ]
# ///
"""
beamform — run the dual-mic voice-beamforming prototypes on a stereo capture (or
a synthetic scene) and score them.

Methods (see dualmic.py / README.md):
  sum   fixed broadside delay-sum beam (linear baseline)
  A     SUM + NLMS adaptive canceller (adapts a null onto a lateral interferer)
  B     IPD/ILD phase-alignment soft mask (broadside selectivity)
  C     coherent-to-diffuse-ratio mask (rejects diffuse/babble/wind/reverb)
  BC    B x C combined  (recommended default)

Usage
  # 1) self-validating demo: build a synthetic scene and score every method
  uv run beamform.py --demo

  # 2) inspect a real capture's geometry (spacing, L/R match, how centred it is)
  uv run beamform.py --calibrate ../lfs_pcm_16000hz_2ch_20260709_010015.wav

  # 3) process a capture with one method (writes <in>_<method>.wav)
  uv run beamform.py ../lfs_pcm_16000hz_2ch_20260709_010015.wav --method BC --spacing-cm 10

  # 4) score all methods against a synthetic scene's ground-truth target
  uv run beamform.py scene.wav --ref scene_target.wav --all --spacing-cm 10
"""
import argparse
import os
import subprocess
import sys
import tempfile

import numpy as np

import dualmic as dm

METHODS = {
    "sum": lambda L, R, fs, d: dm.beam_sum(L, R),
    "A": lambda L, R, fs, d: dm.gsc_lite(L, R, fs=fs),
    "B": lambda L, R, fs, d: dm.ipd_ild_mask(L, R, fs, d_cm=d)[0],
    "C": lambda L, R, fs, d: dm.cdr_mask(L, R, fs, d_cm=d)[0],
    "BC": lambda L, R, fs, d: dm.combined_mask(L, R, fs, d_cm=d)[0],
}


def _apply(name, L, R, fs, d):
    return np.asarray(METHODS[name](L, R, fs, d), dtype=np.float32)


def print_geometry(L, R, fs):
    g = dm.estimate_geometry(L, R, fs)
    print("  geometry / mic match:")
    md = g["delay_samples"]
    if md == md:  # not NaN
        print(f"    median per-frame TDOA  : {md:+.2f} samples (IQR {g['iqr_tdoa']:.2f})"
              f"  <- {'centred/broadside' if abs(md) < 0.5 else 'source off to one side'}")
    print(f"    L/R level mismatch     : {g['gain_db']:+.2f} dB "
          f"(null this with the programmable mic gain)")
    bf = g["broadside_frac"]
    print(f"    voiced frames centred  : {bf * 100:.0f}%  (|TDOA| < 0.25 sample)"
          if bf == bf else "    voiced frames centred  : n/a (no voiced frames)")
    print(f"    global GCC-PHAT delay  : {g['delay_global']:+.2f} samples "
          f"(unreliable on periodic voice; use an endfire snap to measure spacing)")


def run_all(L, R, fs, d, ref=None):
    print(f"\n  {'method':<6} {'out level':>10} {'vs sum':>8}"
          + ("   segSNR(dB)  dSNR" if ref is not None else ""))
    base = dm.level_db(dm.beam_sum(L, R))
    snr_sum = dm.segmental_snr(dm.beam_sum(L, R), ref, fs) if ref is not None else None
    for name in ["sum", "A", "B", "C", "BC"]:
        y = _apply(name, L, R, fs, d)
        lvl = dm.level_db(y)
        row = f"  {name:<6} {lvl:>9.1f}  {lvl - base:>+7.1f}"
        if ref is not None:
            snr = dm.segmental_snr(y, ref, fs)
            row += f"   {snr:>8.2f}  {snr - snr_sum:>+5.2f}"
        print(row)
    if ref is not None:
        print("\n  segSNR = segmental SNR vs the clean broadside target "
              "(higher is better); dSNR = improvement over the plain SUM beam.")


def demo(args):
    """Build a synthetic scene with synth.py, then score every method on it."""
    tmp = tempfile.mkdtemp(prefix="beamform_demo_")
    scene = os.path.join(tmp, "scene.wav")
    here = os.path.dirname(os.path.abspath(__file__))
    cmd = [sys.executable, os.path.join(here, "synth.py"), "--out", scene,
           "--spacing-cm", str(args.spacing_cm),
           "--interferer-deg", str(args.interferer_deg),
           "--isnr", str(args.isnr), "--dsnr", str(args.dsnr)]
    print("=== synthesising scene ===")
    subprocess.run(cmd, check=True)
    ref_path = scene.replace(".wav", "_target.wav")

    L, R, fs = dm.read_stereo(scene)
    ref, _ = dm.read_mono(ref_path)
    print("\n=== geometry (as measured back off the synthetic mix) ===")
    print_geometry(L, R, fs)
    print("\n=== scoring all methods ===")
    run_all(L, R, fs, args.spacing_cm, ref=ref)

    if args.write:
        for name in METHODS:
            y = _apply(name, L, R, fs, args.spacing_cm)
            out = os.path.join(os.getcwd(), f"demo_{name}.wav")
            dm.write_mono(out, y, fs)
        dm.write_stereo(os.path.join(os.getcwd(), "demo_input.wav"), L, R, fs)
        print(f"\nwrote demo_*.wav to {os.getcwd()} (listen to input vs BC)")
    print(f"\n(scene + target in {tmp})")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", nargs="?", help="stereo WAV capture")
    ap.add_argument("--method", choices=list(METHODS), default="BC")
    ap.add_argument("--all", action="store_true", help="run+score every method")
    ap.add_argument("--calibrate", metavar="WAV",
                    help="just print geometry / mic-match for a capture and exit")
    ap.add_argument("--ref", help="clean-target WAV for segmental-SNR scoring")
    ap.add_argument("--spacing-cm", type=float, default=13.0)
    ap.add_argument("--out", help="output WAV (default <in>_<method>.wav)")
    ap.add_argument("--demo", action="store_true",
                    help="synthesise a scene and score all methods (no capture needed)")
    ap.add_argument("--write", action="store_true", help="(demo) also write WAVs")
    # demo scene params
    ap.add_argument("--interferer-deg", type=float, default=60.0)
    ap.add_argument("--isnr", type=float, default=3.0)
    ap.add_argument("--dsnr", type=float, default=6.0)
    args = ap.parse_args()

    if args.demo:
        demo(args)
        return

    if args.calibrate:
        L, R, fs = dm.read_stereo(args.calibrate)
        print(f"{args.calibrate}  (fs={fs}, {len(L)/fs:.1f}s)")
        print_geometry(L, R, fs)
        return

    if not args.input:
        ap.error("need an input WAV, or --demo, or --calibrate WAV")

    L, R, fs = dm.read_stereo(args.input)
    print(f"{args.input}  (fs={fs}, {len(L)/fs:.1f}s, d={args.spacing_cm}cm)")
    print_geometry(L, R, fs)
    ref = dm.read_mono(args.ref)[0] if args.ref else None

    if args.all:
        run_all(L, R, fs, args.spacing_cm, ref=ref)
        return

    y = _apply(args.method, L, R, fs, args.spacing_cm)
    out = args.out or args.input.replace(".wav", f"_{args.method}.wav")
    dm.write_mono(out, y, fs)
    print(f"\n  method {args.method}: level {dm.level_db(y):+.1f} dB "
          f"(sum {dm.level_db(dm.beam_sum(L, R)):+.1f} dB) -> {out}")
    if ref is not None:
        print(f"  segmental SNR vs target: {dm.segmental_snr(y, ref, fs):.2f} dB")


if __name__ == "__main__":
    main()
