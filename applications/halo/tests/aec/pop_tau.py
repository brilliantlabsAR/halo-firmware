#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["numpy"]
# ///
"""Measure the pop: hard-transient window length (rail+click) and the DC-ramp
decay time toward steady DC. Report the sample index by which:
  - |sample| leaves the rail (<32000) : end of hard clip
  - 5ms-smoothed DC comes within 300 counts of steady DC : 'DC quiet'
  - 5ms-smoothed |AC| falls below 3x steady RMS : 'audio quiet'
"""
import sys, wave, numpy as np
def load(p):
    w=wave.open(p,'rb'); ch,r=w.getnchannels(),w.getframerate()
    x=np.frombuffer(w.readframes(w.getnframes()),np.int16).astype(float).reshape(-1,ch); w.close(); return x,r,ch
for p in sys.argv[1:]:
    try: x,r,ch=load(p)
    except Exception as e: print(f"{p}: ERR {e}"); continue
    n=x.shape[0]
    print(f"\n{p.split('/')[-1]} r={r} ch={ch} dur={1000*n/r:.0f}ms")
    for c in range(ch):
        s=x[:,c]
        ss=s[int(r*1.0):min(n,int(r*3.0))]; dc=np.median(ss); rms=np.sqrt(np.mean((ss-dc)**2))
        # end of hard clip
        hot=np.where(np.abs(s[:int(r*0.05)])>=32000)[0]
        clip_end = (hot[-1]+1) if len(hot) else 0
        # DC quiet: 5ms smoothed within 300 of dc, staying
        win=max(1,int(r*0.005)); mov=np.convolve(s[:int(r*0.4)],np.ones(win)/win,'valid')
        out=np.where(np.abs(mov-dc)>=max(300,0.2*abs(dc)))[0]; dcq=(out[-1]+1) if len(out) else 0
        # audio quiet (abs AC): rectified 5ms smoothed under 3x rms
        acmov=np.convolve(np.abs(s[:int(r*0.4)]-dc),np.ones(win)/win,'valid')
        aout=np.where(acmov>=max(3*rms,300))[0]; aq=(aout[-1]+1) if len(aout) else 0
        print(f"  ch{c}: dc={dc:.0f} rms={rms:.0f} | clip_end={clip_end}smp({1000*clip_end/r:.1f}ms) "
              f"DC-quiet={1000*dcq/r:.0f}ms audio-quiet={1000*aq/r:.0f}ms")
