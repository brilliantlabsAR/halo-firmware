import sys, json, wave
import numpy as np

wav_path, ev_path, label = sys.argv[1], sys.argv[2], sys.argv[3]
GATE = 150.0  # the RMS gate threshold under test

w = wave.open(wav_path); SR = w.getframerate()
x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64)

def ac_rms(t0_ms, t1_ms):
    a = max(0, int(t0_ms/1000*SR)); b = min(len(x), int(t1_ms/1000*SR))
    if b - a < 2: return 0.0
    seg = x[a:b]
    # peak AC-RMS over 30ms sub-frames (same idea as the harness barge measure)
    FR = int(0.03*SR); n=(b-a)//FR
    if n < 1: return float(np.std(seg))
    return float(max(np.std(seg[i*FR:(i+1)*FR]) for i in range(n)))

ev = [json.loads(l) for l in open(ev_path) if l.strip()]
def evs(name): return [e for e in ev if e["event"]==name]
dur = ev[-1]["t"]
bi = evs("client_barge_in")
rd = evs("response_done")
cancelled = [e for e in rd if e.get("status")=="cancelled"]
sp = evs("speech_started")

# classify each barge by near-end AC-RMS in the wav window ending at its mic_ms
rows=[]
for e in bi:
    mic_ms = e["mic_ms"]
    rms = ac_rms(mic_ms-250, mic_ms+50)   # ~300ms window around the barge
    rows.append((e["t"], mic_ms, e.get("age_ms",0), e.get("frames",0), rms))

cutting=[r for r in rows if r[3]>0]
empty=[r for r in rows if r[3]==0]
cut_false=[r for r in cutting if r[4]<GATE]   # cut active playback w/ near-end BELOW gate = false cutoff
cut_real =[r for r in cutting if r[4]>=GATE]
frames_cut_false=sum(r[3] for r in cut_false)
frames_cut_total=sum(r[3] for r in cutting)

print(f"===== {label} =====  ({dur:.0f}s, wav {len(x)/SR:.0f}s)")
print(f"replies(response_done)={len(rd)}  server_cancel={len(cancelled)}  speech_started={len(sp)}")
print(f"client_barge_in total={len(bi)}  | cutting active playback={len(cutting)} (dropped {frames_cut_total*10/1000:.1f}s)  | flushed empty={len(empty)}")
print(f"  cutting barges with near-end BELOW {GATE:.0f} RMS (FALSE cutoffs the gate blocks): {len(cut_false)}  (={frames_cut_false*10/1000:.1f}s of reply killed while ~silent)")
print(f"  cutting barges with near-end >= {GATE:.0f} RMS (real barges): {len(cut_real)}")
print("  --- cutting barge detail (t, age_ms, frames, near-end_AC_RMS, verdict) ---")
for r in sorted(cutting):
    verdict = "REAL" if r[4]>=GATE else ">>> FALSE (gate would block)"
    print(f"    t={r[0]:6.1f}  age={r[2]:5}ms  frames={r[3]:4}  rms={r[4]:7.1f}   {verdict}")
# also show empties with high rms (real barges that hit an already-empty queue = harmless)
hi_empty=[r for r in empty if r[4]>=GATE]
print(f"  (empty-queue barges with real near-end voice: {len(hi_empty)} — harmless, reply already done)")
