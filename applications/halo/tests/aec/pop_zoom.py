#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["numpy"]
# ///
"""Zoom the very first samples of each mic WAV: per-sample for 3ms, then
measure (1) hard-rail run length at the very start, (2) DC-settle tau of the
decimator ramp toward steady DC."""
import sys, wave, numpy as np

def load(path):
    w = wave.open(path,'rb'); ch,rate=w.getnchannels(),w.getframerate()
    x=np.frombuffer(w.readframes(w.getnframes()),dtype=np.int16).astype(np.float64).reshape(-1,ch)
    w.close(); return x,rate,ch

for p in sys.argv[1:]:
    try:
        x,rate,ch=load(p); n=x.shape[0]
    except Exception as e:
        print(f"{p}: ERR {e}"); continue
    print(f"\n=== {p.split('/')[-1]} rate={rate} ch={ch} dur={1000*n/rate:.0f}ms")
    for c in range(ch):
        s=x[:,c]
        ss_dc=np.median(s[int(rate*1.0):min(n,int(rate*3.0))])
        # rail run: count leading samples at |.|>=32000
        railed=np.where(np.abs(s[:64])>=32000)[0]
        rail_run = 0
        if len(railed):
            # contiguous run from wherever the first rail is (allow it not at idx0)
            first=railed[0]; k=first
            while k<len(s) and abs(s[k])>=32000: k+=1
            rail_run=k-first
            print(f"  ch{c}: RAIL first@{first}({1000*first/rate:.2f}ms) run={rail_run}smp ({1000*rail_run/rate:.2f}ms) vals={s[first:first+rail_run].astype(int).tolist()}")
        else:
            print(f"  ch{c}: no hard rail in first 64 smp")
        # per-sample first 3ms
        m=int(rate*0.003)
        print(f"        first {m} smp: {s[:m].astype(int).tolist()}")
        # DC ramp: 1ms moving avg over first 120ms, find where it enters +/-8% of ss_dc and stays
        win=max(1,int(rate*0.001))
        mov=np.convolve(s[:int(rate*0.12)],np.ones(win)/win,'valid')
        band=max(60.0,0.08*abs(ss_dc))
        outside=np.where(np.abs(mov-ss_dc)>=band)[0]
        settle=(outside[-1]+1) if len(outside) else 0
        # start value of ramp (avg of samples 3..8 after rail)
        st=int(rail_run)+3; startval=np.mean(s[st:st+16])
        print(f"        ss_dc={ss_dc:.0f} ramp_start~{startval:.0f} DC-settle(8%)~{1000*settle/rate:.1f}ms")
