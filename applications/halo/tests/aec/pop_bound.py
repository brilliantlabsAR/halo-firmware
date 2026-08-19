#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["numpy"]
# ///
"""Worst-case bound on the startup transient across many cold-start WAVs.
For each channel: leading-zero count, transient-start (first |AC|>8*rms after
zeros), rail-end (last |x|>=32000 in first 60ms), and transient-end (last index
in first 60ms where 2ms-rectified-AC > 8*rms, i.e. when it rejoins ambient)."""
import sys, wave, numpy as np
def load(p):
    w=wave.open(p,'rb'); ch,r=w.getnchannels(),w.getframerate()
    x=np.frombuffer(w.readframes(w.getnframes()),np.int16).astype(float).reshape(-1,ch); w.close(); return x,r
worst=0.0; worstf=""
for p in sys.argv[1:]:
    try: x,r=load(p)
    except Exception as e: print(f"{p}: ERR {e}"); continue
    for c in range(x.shape[1]):
        s=x[:,c]; n=len(s)
        dc=np.median(s[int(r*1.0):min(n,int(r*3.0))]); rms=np.sqrt(np.mean((s[int(r*1.0):min(n,int(r*3.0))]-dc)**2))
        rms=max(rms,20.0)
        lead=0
        while lead<len(s) and s[lead]==0: lead+=1
        w60=int(r*0.06); seg=np.abs(s[:w60]-dc)
        rail=np.where(np.abs(s[:w60])>=32000)[0]; rail_end=(rail[-1]+1) if len(rail) else 0
        win=max(1,int(r*0.002)); rect=np.convolve(seg,np.ones(win)/win,'valid')
        hot=np.where(rect>8*rms)[0]; tend=(hot[-1]+1) if len(hot) else 0
        tms=1000*tend/r
        if tms>worst: worst=tms; worstf=f"{p.split('/')[-1]} ch{c}"
        print(f"{p.split('/')[-1]:52s} ch{c} lead={lead:2d} rail_end={1000*rail_end/r:5.1f}ms trans_end={tms:5.1f}ms (dc={dc:.0f} rms={rms:.0f})")
print(f"\nWORST transient_end = {worst:.1f}ms  [{worstf}]")
