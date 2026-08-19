"""Raw timeline + coarse mic-energy envelope + AEC suppressor gain over time."""
import sys, json, wave
import numpy as np

wav_path, ev_path = sys.argv[1], sys.argv[2]
w = wave.open(wav_path); SR = w.getframerate()
x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float32) / 32768.0
dur = len(x) / SR

ev = [json.loads(l) for l in open(ev_path) if l.strip()]

# --- compact turn timeline (non-stats events) ---
print("=== turn timeline (s) ===")
for e in ev:
    if e['event'] in ('config', 'aec_stats_mid'):
        continue
    extra = ''
    if e['event'] == 'client_barge_in':
        extra = f" age_ms={e.get('age_ms')}"
    print(f"  {e['t']:7.1f}  {e['event']}{extra}")

# --- coarse mic RMS envelope: peak dBFS per 5s bin ---
print("\n=== mic peak dBFS per 5s bin (H=has speech-level >-45, .=quiet, _=silent<-80) ===")
BIN = 5.0
nb = int(np.ceil(dur / BIN))
FR = int(0.03 * SR)
line = ''
labels = []
for b in range(nb):
    a0, a1 = int(b*BIN*SR), min(int((b+1)*BIN*SR), len(x))
    seg = x[a0:a1]
    if len(seg) < FR:
        pk = -99
    else:
        nfr = len(seg)//FR
        r = np.array([np.sqrt(np.mean(seg[i*FR:(i+1)*FR]**2))+1e-9 for i in range(nfr)])
        pk = 20*np.log10(r.max())
    c = 'H' if pk > -45 else ('.' if pk > -80 else '_')
    line += c
    if b % 12 == 0:
        labels.append((len(line)-1, int(b*BIN)))
print('  ' + line)
# print a time ruler
ruler = [' '] * len(line)
for pos, t in labels:
    s = str(t)
    for i, ch in enumerate(s):
        if pos+i < len(ruler): ruler[pos+i] = ch
print('  ' + ''.join(ruler) + '  (seconds)')

# --- mic level split by speaker-active vs speaker-idle ---
pb_start = [e['t'] for e in ev if e['event']=='playback_start']
pb_drain = [e['t'] for e in ev if e['event']=='playback_drained']
def active(t):
    for s in pb_start:
        d = min([x for x in pb_drain if x>=s], default=1e9)
        if s <= t <= d: return True
    return False
FR = int(0.03*SR); nfr=len(x)//FR
act_db, idle_db = [], []
for i in range(nfr):
    t = i*0.03
    r = np.sqrt(np.mean(x[i*FR:(i+1)*FR]**2))+1e-9
    (act_db if active(t) else idle_db).append(20*np.log10(r))
import statistics as st
print(f"\n=== mic level: speaker-ACTIVE vs speaker-IDLE frames ===")
if act_db:
    print(f"  speaker ACTIVE ({len(act_db)*0.03:5.0f}s): median {st.median(act_db):6.1f}  p95 {np.percentile(act_db,95):6.1f} dBFS")
if idle_db:
    print(f"  speaker IDLE   ({len(idle_db)*0.03:5.0f}s): median {st.median(idle_db):6.1f}  p95 {np.percentile(idle_db,95):6.1f} dBFS")

# --- suppressor gain (sup_gmean) over time from aec_stats_mid ---
print("\n=== AEC suppressor sup_gmean over time (1.0=passthrough, <1=suppressing) ===")
for e in ev:
    if e['event']=='aec_stats_mid':
        print(f"  {e['t']:7.1f}  sup_gmean={float(e['sup_gmean']):.3f}  sup_gate_fast={float(e['sup_gate_fast']):.2f}  p_mic={float(e['p_mic']):.2e}  p_ref={float(e['p_ref']):.2e}")
