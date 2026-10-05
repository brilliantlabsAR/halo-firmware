#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10,<3.13"
# dependencies = ["numpy", "torch", "torchaudio"]
# ///
"""Silero VAD v5 (the server's barge-in oracle, threshold 0.6) over WAVs, as
tests/aec/silero_score.py. Prints one JSON line {path: {fire, mean}}:
fire = fraction of 1 s windows whose max speech prob >= 0.6.

  uv run silero_vad.py a.wav b.wav ...
"""
import json
import sys
import wave

import numpy as np
import torch

model, _ = torch.hub.load("snakers4/silero-vad", "silero_vad", trust_repo=True, verbose=False)
res = {}
for path in sys.argv[1:]:
    w = wave.open(path)
    sr = w.getframerate()
    x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float32) / 32768.0
    model.reset_states()
    probs = []
    with torch.no_grad():
        for i in range(0, len(x) - 512, 512):
            probs.append(model(torch.from_numpy(x[i:i + 512]), sr).item())
    probs = np.array(probs)
    per = int(sr / 512)
    persec = np.array([probs[j:j + per].max() for j in range(0, len(probs), per)])
    res[path] = dict(fire=float((persec >= 0.6).mean()), mean=float(probs.mean()))
print(json.dumps(res))
