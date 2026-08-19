#!/usr/bin/env python3
"""Score WAV(s) with the server's VAD (Silero v5, thresh 0.6) - the barge-in
oracle for offline suppressor A/B. Reports the fraction of seconds that would
fire a false barge-in, plus mean prob.

Run with a venv that has torch + the cached Silero model:
  python silero_score.py out_a.wav out_b.wav ...

Lower sec>=0.6 = fewer self-interruptions. On the barge scenario the raw echo
scores ~66%; the current-fw AEC output ~3%; a working sustained scramble ~0%.
"""
import sys, wave
import numpy as np
import torch

model, _ = torch.hub.load("snakers4/silero-vad", "silero_vad", trust_repo=True)
for path in sys.argv[1:]:
    w = wave.open(path); sr = w.getframerate()
    x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float32) / 32768.0
    model.reset_states()
    probs = []
    with torch.no_grad():
        for i in range(0, len(x) - 512, 512):
            probs.append(model(torch.from_numpy(x[i:i+512]), sr).item())
    probs = np.array(probs)
    per = int(sr / 512)
    persec = np.array([probs[j:j+per].max() for j in range(0, len(probs), per)])
    fire = int((persec >= 0.6).sum())
    print(f"{path.split('/')[-1]:30s} dur={len(x)/sr:5.1f}s  "
          f"sec>=0.6: {fire:3d}/{len(persec):3d} ({100*fire/max(1,len(persec)):3.0f}%)  "
          f"meanprob={probs.mean():.3f}")
