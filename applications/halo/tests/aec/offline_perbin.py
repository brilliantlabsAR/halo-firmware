#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9,<3.13"
# dependencies = [
#     "lc3py",
#     "numpy",
#     "scipy",
# ]
# ///
"""
Offline validation of the per-bin FDAF (audio_aec.c, CONFIG_HALO_AUDIO_AEC_FDAF)
against REAL device captures, before flashing.

Reconstructs the (mic, reference) streams of the erle_voice_test.py
sessions: the far-end stimulus is the best 14s slice of the Gemini
far-end clip, LC3-encoded and decoded exactly as the device played it,
aligned to each voice_off_*.wav (AEC-off duplex mic capture) by
cross-correlation. Then runs a numpy mirror of the firmware FDAF over
the streams and reports voice-active in-band ERLE, per-octave-band
ERLE, and the coherence-derived LTI bound for the same data.

  uv run offline_perbin.py --clip <far-end reference wav>
"""

import argparse
import glob
import wave

import lc3
import numpy as np
from scipy.signal import coherence, lfilter

SR = 16000
FRAME_MS = 10
LC3_FRAME_BYTES = 40
N, H, K = 1024, 320, 3
BIN_LO, BIN_HI = 20, 243
MU, LEAK, EPS_REL = 0.25, 0.998, 1e-3
HPF_A, LPF_A = 0.1111, 0.7757
POW_A = 0.33
DTD_TH = 2.0
GATE_RMS, HANGOVER = 2e-3, 8
RAMP = 8000
YCLAMP_P, PMIC_A = 9.0, 1.0 / (0.05 * SR)

# Residual suppressor defaults (firmware mirror; see run_fdaf's sup arg)
# exp: gain rule g = max(1 - beta*(Sy/Se)^exp, floor). exp=0.5 is
# amplitude-domain over-subtraction (aec.py stage 3), exp=1.0 is
# power-domain (Wiener-like) - much gentler on near-end speech during
# double-talk for the same echo-dominated suppression depth.
# sy_from_Y mirrors the firmware: Sy is measured on the prediction
# spectrum Y the FDAF already computes (saves a frame buffer + FFT on
# device); measured equal or slightly better than an exact y-history
# frame on every capture, near-end cost unchanged.
SUP_DEFAULTS = dict(beta=1.5, floor=0.1, D=128, pow_a=0.33,
                    att=0.7, rel=0.3, exp=1.0, sy_from_Y=True)
# Cold-onset mu schedule: mu = MU + (MU_HOT - MU) * exp(-adapted/TAU)
MU_HOT, MU_TAU = 1.0, 8000.0
# History-fill guard (firmware FD_HIST_FILL_HOPS): no adaptation while
# the reference history frames still contain the wipe's hard zero edge
# - its rectangular-gating leakage poisons the per-bin gradient
FILL_HOPS = (N + (K - 1) * H + H - 1) // H


def read_wav(path):
    with wave.open(path) as w:
        assert w.getframerate() == SR and w.getnchannels() == 1
        return np.frombuffer(w.readframes(w.getnframes()),
                             dtype=np.int16).astype(np.float64) / 32768.0


def bandpass(x, lo=300.0, hi=3400.0):
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1.0 / SR)
    X[(f < lo) | (f > hi)] = 0
    return np.fft.irfft(X, n=len(x))


def build_stimulus(clip_path, seconds=14.0):
    """load_clip + LC3 round trip, mirroring erle_voice_test.py."""
    pcm = (read_wav(clip_path) * 32768.0).astype(np.int16)
    x = pcm / 32768.0
    xb = bandpass(x) ** 2
    win = int(seconds * SR)
    c = np.cumsum(xb)
    starts = np.arange(0, len(x) - win, SR // 2)
    best = int(starts[np.argmax(c[starts + win] - c[starts])])
    sl = pcm[best:best + win]
    sl = sl[:len(sl) - len(sl) % (SR * FRAME_MS // 1000)]

    enc = lc3.Encoder(FRAME_MS * 1000, SR, 1)
    dec = lc3.Decoder(FRAME_MS * 1000, SR, 1)
    spf = SR * FRAME_MS // 1000
    out = []
    for i in range(0, len(sl), spf):
        blob = enc.encode(sl[i:i + spf].tobytes(), LC3_FRAME_BYTES,
                          bit_depth=16)
        out.append(np.frombuffer(dec.decode(blob, bit_depth=16),
                                 dtype=np.int16))
    return np.concatenate(out).astype(np.float64) / 32768.0


def align(mic, ref):
    """Global lag of ref within mic (first-differenced, band-passed
    xcorr), plus a windowed drift check. Returns (lag, drift_ok, drift).
    """
    def prep(x):
        d = np.diff(bandpass(x))
        return d / (np.std(d) + 1e-12)

    m, r = prep(mic), prep(ref)
    n = len(m) + len(r)
    F = np.fft.rfft(m, n) * np.conj(np.fft.rfft(r, n))
    xc = np.fft.irfft(F, n)
    lag = int(np.argmax(xc))
    if lag > len(m):
        lag -= n  # negative lag (ref starts before capture: unexpected)

    # drift: repeat in 2s windows over the overlap
    lags = []
    for s in range(0, len(ref) - 2 * SR, 2 * SR):
        seg_m = m[max(0, s + lag):s + lag + 2 * SR]
        seg_r = r[s:s + 2 * SR]
        if len(seg_m) < SR:
            continue
        nn = len(seg_m) + len(seg_r)
        cc = np.fft.irfft(np.fft.rfft(seg_m, nn) *
                          np.conj(np.fft.rfft(seg_r, nn)), nn)
        k = int(np.argmax(cc))
        if k > len(seg_m):
            k -= nn
        lags.append(k)
    drift = (max(lags) - min(lags)) if lags else None
    return lag, (drift is not None and drift <= 16), drift


def one_pole_lp(x, a, zi=0.0):
    return lfilter([a], [1.0, -(1.0 - a)], x, zi=[zi * (1.0 - a)])[0]


def update_view(x):
    """The firmware's streaming band-pass: 2x one-pole HP at ~300Hz then
    3x one-pole LP at ~3.8kHz (AEC_HPF_ALPHA / AEC_LPF_ALPHA)."""
    v = x - one_pole_lp(x, HPF_A)
    v = v - one_pole_lp(v, HPF_A)
    for _ in range(3):
        v = one_pole_lp(v, LPF_A)
    return v


def run_fdaf(mic, ref, hot_mu=False, sup=None):
    """Numpy mirror of fd_process_block over aligned full streams.

    hot_mu: cold-onset schedule (exp settle from MU_HOT to MU) instead of
            the 0.5s linear soft-start.
    sup:    residual-suppressor config dict (SUP_DEFAULTS) or None. The
            suppressed output is DELAYED by sup['D'] samples (the causal
            gain-kernel group delay); returned as-is, callers align.
    """
    nblk = min(len(mic), len(ref)) // H
    mic = mic[:nblk * H]
    ref = ref[:nblk * H]
    reff = update_view(ref)

    # per-sample states vectorized where LTI; per-block loop for the rest
    d_hp = update_view_hp2(mic)
    p_mic = lfilter([PMIC_A], [1.0, -(1.0 - PMIC_A)], d_hp ** 2)

    X = np.zeros((K, N // 2 + 1), complex)
    Xf = np.zeros((K, N // 2 + 1), complex)
    W = np.zeros((K, N // 2 + 1), complex)
    P = np.zeros(N // 2 + 1)
    out = np.zeros(nblk * H)
    p_ref = p_err = 0.0
    hang, ramp = 0, 0
    adapted = 0     # samples adapted so far (hot-mu schedule clock)
    err_zi = ErrFilter()
    bins = slice(BIN_LO, BIN_HI + 1)
    frame = 0

    # suppressor state (firmware mirror: res/y history frames, smoothed
    # per-bin powers, previous gains for the attack/release smoothing)
    if sup:
        res_hist = np.zeros(N)
        y_hist = np.zeros(N)
        Sy = np.zeros(N // 2 + 1)
        Se = np.zeros(N // 2 + 1)
        gp = np.ones(N // 2 + 1)
        kwin = np.hanning(2 * sup["D"] + 1)

    for b in range(nblk):
        s = b * H
        xfrm = ref[max(0, s + H - N):s + H]
        xffrm = reff[max(0, s + H - N):s + H]
        if len(xfrm) < N:
            xfrm = np.pad(xfrm, (N - len(xfrm), 0))
            xffrm = np.pad(xffrm, (N - len(xffrm), 0))
        slot = frame % K
        X[slot] = np.fft.rfft(xfrm)
        Xf[slot] = np.fft.rfft(xffrm)

        idx = [(frame + K - k) % K for k in range(K)]
        Y = np.sum(W[np.arange(K)] * X[idx], axis=0)
        y = np.fft.irfft(Y, n=N)[N - H:]

        # y clamp against the high-passed mic power
        lim = np.sqrt(YCLAMP_P * p_mic[s:s + H])
        y = np.clip(y, -lim, lim)

        d = mic[s:s + H]
        e = d - y
        out[s:s + H] = e
        e_f = err_zi(e)

        E = np.fft.rfft(np.concatenate([np.zeros(N - H), e_f]))

        inst = np.sum(np.abs(Xf[idx]) ** 2, axis=0)
        P += POW_A * (inst - P)
        pr = float(np.sum(np.abs(Xf[slot][bins]) ** 2))
        pe = float(np.sum(np.abs(E[bins]) ** 2))
        p_ref += POW_A * (pr - p_ref)
        p_err += POW_A * (pe - p_err)

        # ref gate on the raw new block
        blk = ref[s:s + H]
        if np.mean(blk ** 2) > GATE_RMS ** 2:
            hang = HANGOVER
        elif hang:
            hang -= 1

        if hang and b >= FILL_HOPS and p_ref > 1e-8 and p_err < DTD_TH * p_ref:
            if hot_mu:
                # cold-onset schedule: start hot, settle exponentially;
                # per-bin max(P,inst) normalization bounds each step
                mu = MU + (MU_HOT - MU) * np.exp(-adapted / MU_TAU)
            else:
                mu = MU
                if ramp < RAMP:
                    ramp = min(ramp + H, RAMP)
                    mu = MU * ramp / RAMP
            adapted += H
            eps = EPS_REL * float(np.mean(P[bins])) + 1e-12
            g = mu / (np.maximum(P[bins], inst[bins]) + eps)
            for k in range(K):
                W[k, bins] = (W[k, bins] * LEAK +
                              g * np.conj(Xf[idx[k]][bins]) * E[bins])

        # constraint, round-robin
        c = slot
        w_t = np.fft.irfft(W[c], n=N)
        w_t[H:] = 0
        W[c] = np.fft.rfft(w_t)
        frame += 1

        # ---- residual suppressor (after adaptation; output-path only) --
        if sup:
            res_hist = np.concatenate([res_hist[H:], e])
            R = np.fft.rfft(res_hist)
            if sup.get("sy_from_Y"):
                # firmware memory trim: reuse the prediction spectrum Y
                # (its IFFT's first N-H samples are circularly aliased,
                # so |Y|^2 is only an approximate frame power)
                Syi = np.abs(Y) ** 2
            else:
                y_hist = np.concatenate([y_hist[H:], y])
                Syi = np.abs(np.fft.rfft(y_hist)) ** 2
            Sy += sup["pow_a"] * (Syi - Sy)
            Se += sup["pow_a"] * (np.abs(R) ** 2 - Se)

            g_raw = np.ones(N // 2 + 1)
            ratio = sup["beta"] * (Sy[bins] / (Se[bins] + 1e-14)) ** sup["exp"]
            g_raw[bins] = np.maximum(1.0 - ratio, sup["floor"])
            # fast attack (more suppression), slower release
            down = g_raw < gp
            gp[down] += sup["att"] * (g_raw[down] - gp[down])
            gp[~down] += sup["rel"] * (g_raw[~down] - gp[~down])

            # constrain the zero-phase gain kernel to a short causal
            # support [0, 2D] (rotate by D, Hann taper): the overlap-save
            # product stays alias-free and the audio picks up a constant
            # D-sample group delay
            kern = np.fft.irfft(gp, n=N)
            kern = np.roll(kern, sup["D"])
            kc = np.zeros(N)
            kc[:2 * sup["D"] + 1] = kern[:2 * sup["D"] + 1] * kwin
            Gc = np.fft.rfft(kc)
            out[s:s + H] = np.fft.irfft(R * Gc, n=N)[N - H:]

    return out


def update_view_hp2(x):
    v = x - one_pole_lp(x, HPF_A)
    return v - one_pole_lp(v, HPF_A)


class ErrFilter:
    """Streaming per-sample band-pass state carried across blocks."""

    def __init__(self):
        self.z = np.zeros(5)

    def __call__(self, e):
        z = self.z
        lp1 = lfilter([HPF_A], [1.0, -(1.0 - HPF_A)], e, zi=[z[0]])
        v = e - lp1[0]
        self.z[0] = lp1[1][0]
        lp2 = lfilter([HPF_A], [1.0, -(1.0 - HPF_A)], v, zi=[z[1]])
        v = v - lp2[0]
        self.z[1] = lp2[1][0]
        for i in range(3):
            lpi = lfilter([LPF_A], [1.0, -(1.0 - LPF_A)], v, zi=[z[2 + i]])
            v = lpi[0]
            self.z[2 + i] = lpi[1][0]
        return v


def score(mic, out, label, bands=True):
    """Voice-active in-band ERLE + per-band, mirroring erle_voice_test
    (voice-active = frames 10dB above the quietest-decile frame energy),
    plus onset windows (first 1s / first 3s) - the barge-in gap metric.
    """
    hop = SR // 20
    results = {}
    band_list = [(300, 3400, "300-3400")]
    if bands:
        band_list += [(300, 800, "300-800"), (800, 1600, "800-1600"),
                      (1600, 3400, "1600-3400")]
    for lo, hi, name in band_list:
        mb = bandpass(mic, lo, hi)
        ob = bandpass(out, lo, hi)
        m = len(mb) // hop
        me = (mb[:m * hop].reshape(m, hop) ** 2).mean(axis=1)
        oe = (ob[:m * hop].reshape(m, hop) ** 2).mean(axis=1)
        thr = np.percentile(me, 10) * 10.0
        act = me > thr
        if act.sum() > 5:
            results[name] = 10 * np.log10(me[act].sum() / oe[act].sum())
        if name == "300-3400":
            for t1, oname in [(1.0, "onset-1s"), (3.0, "onset-3s")]:
                w = act[:int(t1 * SR / hop)]
                if w.sum() > 2:
                    results[oname] = 10 * np.log10(
                        me[:len(w)][w].sum() / oe[:len(w)][w].sum())
    parts = ", ".join(f"{k}: {v:+.1f}" for k, v in results.items())
    print(f"  {label}: {parts} dB")
    return results


def lti_bound(mic, ref):
    """Coherence-derived LTI cancellation bound, mic-PSD-weighted."""
    from scipy.signal import welch
    f, cxy = coherence(mic, ref, fs=SR, nperseg=1024, noverlap=768)
    fw, pm = welch(mic, fs=SR, nperseg=1024, noverlap=768)
    for lo, hi in [(300, 3400), (300, 800), (800, 1600), (1600, 3400)]:
        m = (f >= lo) & (f <= hi)
        g2 = np.clip(cxy[m], 0, 0.9999)
        w = pm[m]
        bound = 10 * np.log10(np.sum(w) / np.sum(w * (1 - g2)))
        print(f"    LTI bound {lo}-{hi}: {bound:+.1f} dB "
              f"(mean coh {np.mean(g2):.2f})")


def near_end_atten(m, r, near, sup, hot_mu=True):
    """Double-talk preservation: run the FDAF+suppressor on mic and on
    mic+near (a known injected wearer-like signal), then report the
    suppressor's least-squares gain on the near component per 50ms frame
    over near-active frames. 0dB = wearer untouched."""
    D = sup["D"]
    out = run_fdaf(m + near, r, hot_mu=hot_mu, sup=sup)[D:]
    nn = min(len(out), len(near))
    o, ne = out[:nn], near[:nn]
    hop = SR // 20
    fr = nn // hop
    o = o[:fr * hop].reshape(fr, hop)
    ne = ne[:fr * hop].reshape(fr, hop)
    pe = (ne ** 2).mean(axis=1)
    act = pe > np.percentile(pe, 75) * 0.1
    gain = (o[act] * ne[act]).sum(axis=1) / ((ne[act] ** 2).sum(axis=1) + 1e-15)
    gdb = 20 * np.log10(np.abs(gain) + 1e-6)
    return float(np.median(gdb)), float(np.percentile(gdb, 10))


def save_wav(path, x):
    import wave as _w
    with _w.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clip", required=True,
                    help="far-end reference wav (as played by the device)")
    ap.add_argument("--captures", default="voice_off_*.wav")
    ap.add_argument("--sweep", action="store_true",
                    help="sweep suppressor/onset params on each capture")
    ap.add_argument("--write-wavs", action="store_true",
                    help="write *_sup.wav suppressed outputs for listening")
    ap.add_argument("--no-bands", action="store_true")
    ap.add_argument("--dt", action="store_true",
                    help="double-talk preservation: inject a synthetic "
                         "near-end voice and report its attenuation")
    ap.add_argument("--mu-hot", type=float, default=None,
                    help="override MU_HOT for the hot-mu configs")
    args = ap.parse_args()
    if args.mu_hot is not None:
        global MU_HOT
        MU_HOT = args.mu_hot

    stim = build_stimulus(args.clip)
    print(f"stimulus: {len(stim) / SR:.1f}s (LC3 round-tripped)")

    # near-end material for --dt: a DIFFERENT slice of the raw clip
    # (speech-like, uncorrelated with the LC3'd stimulus slice)
    near_src = None
    if args.dt:
        raw = read_wav(args.clip)
        xb = bandpass(raw) ** 2
        c = np.cumsum(xb)
        win = int(14.0 * SR)
        starts = np.arange(0, len(raw) - win, SR // 2)
        best = int(starts[np.argmax(c[starts + win] - c[starts])])
        # take the *second best* non-overlapping region
        mask = (starts < best - win) | (starts > best + win)
        second = int(starts[mask][np.argmax((c[starts + win] - c[starts])[mask])])
        near_src = raw[second:second + win]

    for path in sorted(glob.glob(args.captures)):
        mic = read_wav(path)
        mic = mic[int(0.5 * SR):]  # PDM start pop
        lag, ok, drift = align(mic, stim)
        print(f"\n{path}: lag {lag} samples ({lag / SR * 1000:.1f} ms), "
              f"windowed drift {drift} ({'OK' if ok else 'UNSTABLE - skip'})")
        if not ok or lag < 0:
            continue
        # Build aligned streams over the playback region, placing the
        # echo ~160 taps INTO the filter window (like the device's
        # hold-back + REF_LEAD geometry does): with the xcorr peak at
        # tap 0, any IR precursor would fall outside the causal span.
        margin = 160
        nn = min(len(mic) - lag, len(stim) - margin)
        m = mic[lag:lag + nn]
        r = stim[margin:margin + nn]

        bands = not args.no_bands
        out0 = run_fdaf(m, r)
        score(m, out0, "baseline (current fw)   ", bands)
        out1 = run_fdaf(m, r, hot_mu=True)
        score(m, out1, "hot-mu onset            ", bands)
        D = SUP_DEFAULTS["D"]
        out2 = run_fdaf(m, r, hot_mu=True, sup=SUP_DEFAULTS)
        score(m[:len(out2) - D], out2[D:], "hot-mu + suppressor     ", bands)
        if args.write_wavs:
            base = path.rsplit(".", 1)[0]
            save_wav(f"{base}_sup.wav", out2[D:])
            print(f"  wrote {base}_sup.wav")

        if args.dt and near_src is not None:
            ne = near_src[:len(m)]
            ne = np.pad(ne, (0, len(m) - len(ne)))
            for lvl_db, tag in [(-25.0, "wearer-loud"), (-35.0, "wearer-soft")]:
                band = bandpass(ne)
                cur = 20 * np.log10(np.sqrt(np.mean(band ** 2)) + 1e-12)
                nes = ne * (10 ** ((lvl_db - cur) / 20))
                for beta, ex in ((1.0, 0.5), (1.0, 1.0), (1.5, 1.0),
                                 (2.0, 1.0)):
                    cfg = dict(SUP_DEFAULTS, beta=beta, exp=ex)
                    med, p10 = near_end_atten(m, r, nes, cfg)
                    print(f"  near-end {tag} ({lvl_db:.0f}dBFS) "
                          f"beta={beta} exp={ex}: "
                          f"median {med:+.1f} dB, p10 {p10:+.1f} dB")

        if args.sweep:
            for beta, ex in ((1.0, 0.5), (1.5, 0.5), (1.0, 1.0),
                             (1.5, 1.0), (2.0, 1.0), (3.0, 1.0)):
                cfg = dict(SUP_DEFAULTS, beta=beta, exp=ex)
                o = run_fdaf(m, r, hot_mu=True, sup=cfg)
                score(m[:len(o) - D], o[D:],
                      f"hot=1 beta={beta} exp={ex}   ", bands=False)
            for D2 in (64, 256):
                cfg = dict(SUP_DEFAULTS, D=D2)
                o = run_fdaf(m, r, hot_mu=True, sup=cfg)
                score(m[:len(o) - D2], o[D2:], f"hot=1 D={D2}         ",
                      bands=False)
        lti_bound(m, r)


if __name__ == "__main__":
    main()
