"""Diagnose 'my next-turn speech didn't register': correlate the s2s turn
timeline (events jsonl) with (a) offline Silero VAD on the EXACT captured mic
audio and (b) mic RMS in the window right after each assistant turn ends.

Run with a venv that has torch + numpy (e.g. the realtime VAD server's venv):
  python analyze_turns.py duplex_mic_174216.wav duplex_events_174216.jsonl
"""
import sys, json, wave
import numpy as np
import torch

wav_path, ev_path = sys.argv[1], sys.argv[2]

# --- load audio ---
w = wave.open(wav_path); SR = w.getframerate()
x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float32) / 32768.0
dur = len(x) / SR

# --- RMS envelope, 30ms frames, in dBFS ---
FR = int(0.03 * SR)
nfr = len(x) // FR
rms = np.array([np.sqrt(np.mean(x[i*FR:(i+1)*FR]**2)) + 1e-9 for i in range(nfr)])
rms_db = 20 * np.log10(rms)
def db_at(t):  # mic dBFS at time t
    i = min(int(t / 0.03), nfr - 1)
    return rms_db[i]
def db_window(t0, t1):  # peak dBFS in [t0,t1]
    a, b = int(t0/0.03), min(int(t1/0.03), nfr)
    return float(rms_db[a:b].max()) if b > a else -99.0

# --- Silero VAD (same knobs as s2s: thresh 0.6, min_speech 384, min_sil 64) ---
model, utils = torch.hub.load("snakers4/silero-vad", "silero_vad", trust_repo=True)
get_ts = utils[0]
speech = get_ts(torch.from_numpy(x), model, sampling_rate=SR, threshold=0.6,
                min_speech_duration_ms=384, min_silence_duration_ms=64,
                speech_pad_ms=30)
vad_seg = [(s['start']/SR, s['end']/SR) for s in speech]

# --- parse events ---
ev = [json.loads(l) for l in open(ev_path) if l.strip()]
def times(name): return [e['t'] for e in ev if e['event'] == name]
sp_start = times('speech_started')
pb_start = times('playback_start')
pb_drain = times('playback_drained')
resp_done = times('response_done')

print(f"\naudio {dur:.1f}s  |  Silero speech segments: {len(vad_seg)}  |  "
      f"s2s speech_started: {len(sp_start)}  |  assistant turns (playback_start): {len(pb_start)}")
print(f"overall mic level: median {np.median(rms_db):.1f} dBFS, p95 {np.percentile(rms_db,95):.1f} dBFS")

# --- Silero segments s2s did NOT register (no speech_started within +/-1.5s) ---
print("\n--- Silero-detected speech vs s2s registration ---")
missed = 0
for (s, e) in vad_seg:
    reg = any(abs(s - t) < 1.5 for t in sp_start)
    # nearest assistant-turn-end before this speech
    prior_drain = max([d for d in pb_drain if d <= s + 0.5], default=None)
    gap = (s - prior_drain) if prior_drain is not None else None
    tag = "REGISTERED" if reg else ">>> MISSED"
    if not reg: missed += 1
    gtxt = f"{gap:5.1f}s after turn-end" if gap is not None else "  (no prior turn)"
    print(f"  {tag:12s} speech {s:6.1f}-{e:6.1f}s  peak {db_window(s,e):5.1f}dBFS  {gtxt}")
print(f"\nSilero found {len(vad_seg)} speech segments; {missed} were NOT registered by s2s.")

# --- post-turn-end recovery: mic level in 0-0.5 / 0.5-1.5 / 1.5-3s after each drain ---
print("\n--- mic level after each assistant turn ends (playback_drained) ---")
print("  turn-end     0-0.5s   0.5-1.5s   1.5-3s   next speech_started")
for d in pb_drain:
    nxt = min([t for t in sp_start if t > d], default=None)
    nxt_txt = f"+{nxt-d:.1f}s" if nxt else "(none)"
    print(f"  {d:7.1f}s   {db_window(d,d+0.5):6.1f}  {db_window(d+0.5,d+1.5):8.1f}  "
          f"{db_window(d+1.5,d+3.0):7.1f}   {nxt_txt}")
