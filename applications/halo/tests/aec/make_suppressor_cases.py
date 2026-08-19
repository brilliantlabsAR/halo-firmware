#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy"]
# ///
"""Build the three offline suppressor-eval cases from the AEC-off worn vol-80
capture (duplex_mic_011535 = raw echo, duplex_ref_011535 = its reference).
Each case is a mic WAV to run through host/aec_wav (built with different
suppressor flags), then score with silero_score.py.

  case_echo_mic.wav      raw echo, no near-end/noise
                         -> want Silero ~0% (cap must scramble the echo)
  case_nearend_mic.wav   echo + a near-end speech burst in [76.2,82.6]s
                         -> want the near-end PRESERVED (energy survives)
  case_noise_mic.wav     echo + continuous pink-ish background noise
                         -> want Silero ~0% (a good gate must NOT release on
                            noise; if it does, the echo bleeds and fires)
All share duplex_ref_011535.wav as the reference. Deterministic (no RNG seed
issues): noise is a fixed LCG.

Usage:  uv run make_suppressor_cases.py
Then:   gcc ... -DAEC_SUP_STEADY_GCAP=0.08f [-D<gate flags>] -o aec_wav ...
        ./aec_wav case_<x>_mic.wav duplex_ref_011535.wav out_<x>.wav
        <s2s venv>/python silero_score.py out_*.wav
"""
import wave
import numpy as np

SR = 16000

def load(p):
    w = wave.open(p); return np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64)
def save(p, x):
    x = np.clip(x, -32768, 32767).astype(np.int16)
    w = wave.open(p, "wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
    w.writeframes(x.tobytes()); w.close()
def vb(x, lo=200, hi=3500):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1/SR); X[(f < lo) | (f > hi)] = 0
    return np.fft.irfft(X, n=len(x))

mic = load("duplex_mic_011535.wav")
ne = load("qwen_tts_clip.wav")

# 1. echo-only
save("case_echo_mic.wav", mic)

# 2. echo + near-end burst (target voice-band RMS ~2500 = a clear barge-in)
a, b = int(76.2*SR), int(82.6*SR)
seg = ne[:b-a].copy()
segv = vb(seg); seg *= 2500.0 / (np.sqrt((segv**2).mean()) + 1e-9)
m2 = mic.copy(); m2[a:a+len(seg)] += seg
save("case_nearend_mic.wav", m2)

# 3. echo + continuous background noise (seeded RNG white -> 3-tap MA for a
#    voice-band-ish tilt), voice-band RMS ~2000 (a genuinely noisy room). Loud
#    enough to give the case TEETH: a power gate (option A) releases on it and
#    the echo bleeds (Silero fires); a good envelope gate must stay ~0%. At the
#    quieter ~400 level even the noise-fragile power gate did not release, so
#    the case did not discriminate. Tune here if a gate variant needs a harder
#    or easier noise floor.
n = len(mic)
s = np.random.default_rng(12345).standard_normal(n + 2)
noise = (s[:n] + s[1:n+1] + s[2:n+2]) / 3.0
nv = vb(noise); noise *= 2000.0 / (np.sqrt((nv**2).mean()) + 1e-9)
save("case_noise_mic.wav", mic + noise)

print("wrote case_echo_mic.wav, case_nearend_mic.wav, case_noise_mic.wav")
print("reference for all three: duplex_ref_011535.wav")
