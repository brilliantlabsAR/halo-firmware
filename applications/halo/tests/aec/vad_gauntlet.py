#!/usr/bin/env python3
"""Run the s2s (huggingface/speech-to-speech) Silero VAD over mic captures.

Answers "would this audio fire a (false) barge-in?" offline, with the SAME
code path the live server uses: speech_to_speech's VADIterator plus the
vad_handler confirmation rule - a speech start is only confirmed (and a
barge-in only fires) once a triggered episode accumulates >= min_speech_ms
of active speech (chunks with prob >= threshold - 0.15).

Typical use, against the AEC listening-test captures:

    cd <speech-to-speech checkout> && uv run python \
        <alif>/applications/halo/tests/aec/vad_gauntlet.py \
        --sweep \
        <alif>/applications/halo/tests/aec/voice_off_134816.wav \
        <alif>/applications/halo/tests/aec/voice_on_134816.wav \
        <alif>/applications/halo/tests/aec/voice_on_134924.wav

(run from the speech-to-speech checkout so torch + the cached Silero
model are available; --s2s-src overrides the import path if needed).

Interpretation for the barge-in problem:
  - AEC-on capture with the wearer SILENT (e.g. voice_on_134816): every
    confirmed start is a FALSE barge-in - the goal is zero.
  - AEC-on capture with the wearer TALKING OVER playback (voice_on_134924):
    confirmed starts are the wearer's real speech - these must survive.
  - AEC-off capture (voice_off_134816): control; expect the assistant's
    voice to trigger constantly.
"""

import argparse
import os
import sys
import wave

import numpy as np

CHUNK = 512  # Silero v5 window at 16 kHz, same as the realtime router feeds


def load_wav(path, sr):
    with wave.open(path, "rb") as w:
        assert w.getnchannels() == 1, f"{path}: expected mono"
        assert w.getsampwidth() == 2, f"{path}: expected PCM16"
        assert w.getframerate() == sr, f"{path}: expected {sr} Hz"
        pcm = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
    return pcm.astype(np.float32) / 32768.0


def run_file(model, VADIterator, torch, audio, sr, thresh, min_silence_ms,
             min_speech_ms, speech_pad_ms):
    """Feed the file through VADIterator; return the episode list.

    Each episode: dict(trigger_s, confirm_s or None, end_s, active_ms).
    confirm_s is when the live server would emit speech_started / barge-in.
    """
    it = VADIterator(model, threshold=thresh, sampling_rate=sr,
                     min_silence_duration_ms=min_silence_ms,
                     speech_pad_ms=speech_pad_ms)
    min_speech_samples = int(sr * min_speech_ms / 1000)

    episodes = []
    cur = None
    confirmed = False
    for i in range(0, len(audio) - CHUNK + 1, CHUNK):
        t = i / sr
        out = it(torch.from_numpy(audio[i:i + CHUNK]))

        if it.triggered and cur is None:
            cur = {"trigger_s": t, "confirm_s": None, "end_s": None,
                   "active_ms": 0.0}
            confirmed = False
        if cur is not None and not confirmed and it.triggered \
                and it.active_speech_samples >= min_speech_samples:
            cur["confirm_s"] = t + CHUNK / sr
            confirmed = True
        if out is not None and cur is not None:
            cur["end_s"] = t + CHUNK / sr
            cur["active_ms"] = it.last_utterance_active_speech_samples \
                / sr * 1000
            episodes.append(cur)
            cur = None
    if cur is not None:  # file ended mid-episode
        cur["end_s"] = len(audio) / sr
        cur["active_ms"] = it.active_speech_samples / sr * 1000
        episodes.append(cur)
    return episodes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wavs", nargs="+")
    ap.add_argument("--thresh", type=float, default=0.6,
                    help="primary threshold (s2s default 0.6)")
    ap.add_argument("--sweep", action="store_true",
                    help="also sweep thresholds 0.3..0.9")
    ap.add_argument("--min-silence-ms", type=int, default=64)
    ap.add_argument("--min-speech-ms", type=int, default=384)
    ap.add_argument("--speech-pad-ms", type=int, default=30,
                    help="vad_handler passes 30 (not the dataclass's 500)")
    ap.add_argument("--sample-rate", type=int, default=16000)
    ap.add_argument("--s2s-src", default="src",
                    help="speech-to-speech src dir (default: ./src)")
    args = ap.parse_args()

    sys.path.insert(0, args.s2s_src)
    import torch
    from speech_to_speech.VAD.vad_iterator import VADIterator

    model, _ = torch.hub.load("snakers4/silero-vad", "silero_vad",
                              trust_repo=True, skip_validation=True)

    thresholds = [args.thresh]
    if args.sweep:
        thresholds = sorted(set(
            [round(0.3 + 0.1 * k, 1) for k in range(7)] + [args.thresh]))

    for path in args.wavs:
        audio = load_wav(path, args.sample_rate)
        dur = len(audio) / args.sample_rate
        print(f"\n=== {os.path.basename(path)}  ({dur:.1f} s) ===")

        for thresh in thresholds:
            eps = run_file(model, VADIterator, torch, audio,
                           args.sample_rate, thresh, args.min_silence_ms,
                           args.min_speech_ms, args.speech_pad_ms)
            starts = [e for e in eps if e["confirm_s"] is not None]
            first = f"{starts[0]['confirm_s']:6.2f}s" if starts else "  none "
            trig_ms = sum(e["active_ms"] for e in eps)
            print(f"  thresh {thresh:.1f}: confirmed starts {len(starts):2d} "
                  f"(first {first})  episodes {len(eps):2d}  "
                  f"active speech {trig_ms / 1000:5.1f} s")
            if thresh == args.thresh:
                for e in eps:
                    conf = (f"CONFIRMED @{e['confirm_s']:.2f}s"
                            if e["confirm_s"] is not None else "discarded")
                    print(f"      {e['trigger_s']:6.2f}-{e['end_s']:6.2f}s "
                          f"active {e['active_ms']:5.0f} ms  {conf}")


if __name__ == "__main__":
    main()
