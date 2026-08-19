#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["numpy"]
# ///
"""Characterize the PDM start-of-stream 'pop' from mic recordings.

For each WAV, look at the first N ms per channel and report:
 - global steady-state DC and RMS (measured on a late, quiet-ish window)
 - the onset transient: peak sample & its time, how long until the signal
   settles to within a tolerance band of steady state (settle time), and the
   shape (monotonic DC ramp vs impulsive click) via early-sample dump.
"""
import sys, wave, numpy as np

def load(path):
    w = wave.open(path, 'rb')
    ch, sw, rate = w.getnchannels(), w.getsampwidth(), w.getframerate()
    raw = w.readframes(w.getnframes()); w.close()
    x = np.frombuffer(raw, dtype=np.int16).astype(np.float64)
    x = x.reshape(-1, ch)
    return x, rate, ch

def analyze(path, onset_ms=200):
    x, rate, ch = load(path)
    n = x.shape[0]
    onset = int(rate*onset_ms/1000)
    print(f"\n=== {path.split('/')[-1]}  rate={rate} ch={ch} dur={1000*n/rate:.0f}ms")
    for c in range(ch):
        s = x[:, c]
        # steady-state reference: median DC + RMS over a late window [1.0s,3.0s]
        a, b = int(rate*1.0), min(n, int(rate*3.0))
        ss = s[a:b]
        ss_dc = np.median(ss)
        ss_ac = ss - ss_dc
        ss_rms = np.sqrt(np.mean(ss_ac**2))
        # onset stats (AC removed against steady DC)
        head = s[:onset]
        head_ac = head - ss_dc
        pk_i = int(np.argmax(np.abs(head_ac)))
        pk_v = head_ac[pk_i]
        # settle time: first index after which |running DC - ss_dc| stays < band
        # use 5ms moving average of raw to track DC drift
        win = max(1, int(rate*0.005))
        mov = np.convolve(s[:int(rate*0.4)], np.ones(win)/win, mode='valid')
        band = max(50.0, 0.15*abs(ss_dc) if ss_dc else 50.0)
        dev = np.abs(mov - ss_dc)
        settled = np.where(dev < band)[0]
        # find first index from which it STAYS settled
        settle_ms = None
        if len(settled):
            arr = dev < band
            # last time it was outside band:
            outside = np.where(~arr)[0]
            first_stable = (outside[-1]+1) if len(outside) else 0
            settle_ms = 1000*first_stable/rate
        print(f"  ch{c}: ss_dc={ss_dc:8.1f} ss_rms={ss_rms:7.1f} | "
              f"onset_peak={pk_v:+8.1f} @ {1000*pk_i/rate:5.1f}ms "
              f"(x{abs(pk_v)/max(ss_rms,1):.0f} rms) | DC-settle~{settle_ms}ms band={band:.0f}")
        # early raw dump every 2ms for first 30ms
        step = max(1,int(rate*0.002))
        dump = s[:int(rate*0.03):step].astype(int)
        print(f"        raw@2ms: {list(dump)}")

if __name__ == "__main__":
    for p in sys.argv[1:]:
        try:
            analyze(p)
        except Exception as e:
            print(f"{p}: ERR {e}")
