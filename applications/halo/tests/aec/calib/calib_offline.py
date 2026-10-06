#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10,<3.13"
# dependencies = ["numpy", "lc3py"]
# ///
"""Step 3 of the worn calibration: offline replay + narrowed search.

Turns a session's AEC-off captures into (mic, ref) replay pairs, scores
aec_tune sets by replaying them through this tree's audio_aec.c
(calib_build.py: aec_replay, real tune API) and runs a narrowed search
around the current defaults and A15 (calib_common.NAMED).

  calib_offline.py prep    SESSION               build SESSION/replay/
  calib_offline.py step3   SESSION [--budget 90] [--guard DIR ...|--no-guard]
                                                 prep + fixed sets + search -> results/step3.json
  calib_offline.py eval    SESSION [key=val ...] [--ref]   score one set (default: the device's)

Scoring (all on the wearer's own device and head):
  echo  every AEC-off echo-only capture (step 2a) -> dB removed per band,
        residual over the floor (over10/over20), gate false-release fraction
  syn   echo-only capture + wearer-only capture (step 2b), both real worn
        captures at their real levels; the wearer is also pushed through the
        AEC as a shadow signal, so its kept gain is exact (talker kept,
        crushed fraction). Time to pass after each onset is step 4's method:
        the first output frame over that set's energy-VAD threshold (max(floor
        + 10 dB, p95 of the same set's echo-only residual + 3 dB), from the
        echo replay of the same capture), within 0.8 s; onsets from the
        wearer-only capture; median over all onsets with a miss counted as
        0.8 s (ttfp), plus the missed fraction. 'Full pass' (shadow gain
        within 6 dB) is reported too: it was the gate release time while a
        closed gate capped the talker at about -6..-8 dB (cap_lo_gcap 0.5);
        with cap_lo_gcap 0.6 (-4.4 dB) and voice energy mostly below the
        750 Hz split, a talker under a closed gate can already read within
        6 dB, so it now reads early (ttfp is the onset measure).
  rdt   real wearer-over-echo captures (step 2c): energy method against the
        same clip's echo-only capture (kept)

How the reference is built: the exact LC3 bytes the device played are
decoded (host liblc3), passed through the production speaker_protect.c
(spk_ref, speaker.start{volume, gain, budget}), aligned to the capture by
cross-correlation near the playback command's capture time, and placed 24
samples early (the device's echo sits ~16-32 samples behind its tap). The
replay feeds every 20 ms block of the playback span, as the device's open
speaker session does. Outputs are re-aligned by the AEC's 448-sample delay.
"""
import json
import os
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ProcessPoolExecutor

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import calib_build as CB  # noqa: E402
import calib_common as C  # noqa: E402

LEAD = 24        # reference placed 24 samples early
_TOOLS = {}


def tools():
    if not _TOOLS:
        _TOOLS.update(CB.build_all())
    return _TOOLS


_TREE = None


def tree_defaults():
    """{key: default} of this tree's audio_aec.c (aec_replay -k)."""
    global _TREE
    if _TREE is None:
        r = subprocess.run([tools()["aec_replay"], "-k"], capture_output=True, text=True, check=True)
        _TREE = {}
        for ln in r.stdout.splitlines():
            k, v, _, _, ty = ln.split()
            _TREE[k] = int(v) if ty == "u32" else C.norm(v)
    return _TREE

# ------------------------------------------------------------------ replay


def run(mic, ref, params=None, feed=None, shadow=None, dump=False, gain_scale=1.0):
    """Replay through the firmware AEC. params: a tune table (keys that differ
    from this tree's defaults are applied) or C.REF_SET (the 0.8.17 source).
    mic/ref/shadow: wav paths. gain_scale: the capture's effective mic gain
    relative to gain 1 (session_gain_scale), applied as the device's mic
    stream does (the 0.8.17 source predates it and runs unscaled). Returns
    dict(out, shadow_out, dump), outputs re-aligned to the input."""
    with tempfile.TemporaryDirectory(prefix="aec_") as td:
        if params == C.REF_SET:
            exe = tools()["aec_replay_0817"]
            if exe is None:
                raise RuntimeError("no 0.8.17 build (tag missing)")
            args = [exe]
        else:
            args = [tools()["aec_replay"]]
            d = C.diff(params or {}, tree_defaults())
            if d:
                args += ["-t", ",".join(f"{k}={C.fmt_val(v)}" for k, v in sorted(d.items()))]
            if gain_scale != 1.0:
                args += ["-g", repr(float(gain_scale))]
        if feed is not None:
            args += ["-f", f"{feed[0]},{feed[1]}"]
        if shadow is not None:
            args += ["-s", shadow, "-o", os.path.join(td, "sh_out.wav")]
        if dump:
            args += ["-d", os.path.join(td, "dump.tsv")]
        args += [mic, ref, os.path.join(td, "out.wav")]
        subprocess.run(args, check=True, capture_output=True)
        res = {"out": C.shift(C.read_wav(os.path.join(td, "out.wav")), C.AEC_DELAY)}
        if shadow is not None:
            res["shadow_out"] = C.shift(C.read_wav(os.path.join(td, "sh_out.wav")), C.AEC_DELAY)
        if dump:
            res["dump"] = np.genfromtxt(os.path.join(td, "dump.tsv"), names=True)
        return res


def spk_ref(clip_pcm, vol, gain, budget, out_dir, tag):
    """Speaker tap model: production speaker_protect.c on the host."""
    a = os.path.join(out_dir, f"clip_{tag}.wav")
    b = os.path.join(out_dir, f"ref_spk_{tag}.wav")
    C.write_wav(a, clip_pcm)
    subprocess.run([tools()["spk_ref"], a, b, str(vol), str(gain), str(budget), "0"],
                   capture_output=True, text=True, check=True)
    return C.read_wav(b)

# ---------------------------------------------------------------------- prep


def place(ref, n, lag, lead=LEAD):
    out = np.zeros(n)
    s = lag - lead
    a, b = max(0, s), min(n, s + len(ref))
    if b > a:
        out[a:b] = ref[a - s:b - s]
    fs = (max(0, s) // C.HOP) * C.HOP
    fe_ = min(n, ((s + len(ref) + C.HOP - 1) // C.HOP) * C.HOP)
    return out, fs, fe_


def windowed_lag(x, ref, lo_s, hi_s, window_q=False):
    """xcorr restricted to lags in [lo_s, hi_s] seconds (robust with little echo).
    Returns (lag, peak / median over all lags) and, with window_q, also the
    peak over the median inside the window (see ALIGN_Q_MIN)."""
    a = C.bandpass(np.asarray(x, float), 300, 3400)
    b = C.bandpass(np.asarray(ref, float), 300, 3400)
    n = 1 << int(np.ceil(np.log2(len(a) + len(b))))
    c = np.abs(np.fft.irfft(np.fft.rfft(a, n) * np.conj(np.fft.rfft(b, n)), n)[:len(a)])
    lo, hi = max(0, int(lo_s * C.SR)), min(len(a) - 1, int(hi_s * C.SR))
    k = lo + int(np.argmax(c[lo:hi + 1]))
    q = float(c[k] / (np.median(c) + 1e-12))
    if window_q:
        return k, q, float(c[k] / (np.median(c[lo:hi + 1]) + 1e-12))
    return k, q


# An alignment whose xcorr peak is under this many times the median inside the
# search window is no better than chance: the desk sittings' echo captures
# align at 31-139, captures without the echo (2b, wrong reference) peak at
# 5-8.5. (The all-lags median is no use here: the dt captures are zeroed after
# the first cue, which drives it towards 0.) Such a recording is not scored.
ALIGN_Q_MIN = 15.0


def floor_frames(x, a_s=0.2, b_s=0.9):
    e = C.fe(x)
    return float(np.median(e[int(a_s * 50):int(b_s * 50)]))


def play_start_hint(rec):
    """Capture-time (s) of the device's playback command, from the first diag
    sample (capn = capture bytes so far), if the trial has diag."""
    dg = rec.get("diag") or []
    if dg:
        return dg[0][0] / C.LC3_BPS
    return None


def talker_mask(x, floor):
    e = C.fe(x)
    m = (e > floor * 10) & (e > e.max() * 1e-3)
    m[:10] = False
    return m


def prep(sess_dir):
    """SESSION/replay/: one dir per AEC-off item (mic.wav [+ ref.wav]) + manifest.json."""
    S = C.Session(sess_dir)
    st = S.s["settings"]
    out = os.path.join(sess_dir, "replay")
    os.makedirs(out, exist_ok=True)
    refs = {}
    man = {"lead": LEAD, "session": os.path.abspath(sess_dir), "items": {}}
    floors = []
    for t in S.trials(done=True):
        if t.get("aec") or t["kind"] == "probe":
            continue
        rec = S.load_trial(t["id"])
        x = rec["x"]
        n = (len(x) // C.HOP) * C.HOP
        x = x[:n]
        d = os.path.join(out, t["id"])
        os.makedirs(d, exist_ok=True)
        C.write_wav(os.path.join(d, "mic.wav"), x)
        fl = floor_frames(x)
        item = dict(kind=t["kind"], step=t["step"], n=n, floor=fl, level=t.get("level"))
        if t["kind"] == "floor":
            floors.append(fl)
        if t["kind"] in ("echo", "dt"):
            cid = t["clip"]
            if cid not in refs:
                lc3 = open(os.path.join(sess_dir, "clips", f"{cid}.lc3"), "rb").read()
                refs[cid] = spk_ref(C.lc3_decode(lc3), st["volume"], st["spk_gain"],
                                    st["budget"], out, cid)
            ref = refs[cid]
            hint = play_start_hint(rec)
            xc = x.copy()
            if t["kind"] == "dt":
                # only the echo before the wearer's first possible onset
                t0 = (hint if hint is not None else st.get("lead_s", 1.0))
                first = min(p["at"] for p in t["prompts"])
                xc[int((t0 + first + 0.15) * C.SR):] = 0
            if hint is not None:
                lag, q, qw = windowed_lag(xc, ref, hint - 0.05, hint + 0.6, window_q=True)
            else:
                lag, q = C.xcorr_lag(xc, ref)
                qw = windowed_lag(xc, ref, 0, len(xc) / C.SR, window_q=True)[2]
            r, fs, fe_ = place(ref, n, lag)
            C.write_wav(os.path.join(d, "ref.wav"), r)
            rb, xb = C.bandpass(r, 300, 3400), C.bandpass(x, 300, 3400)
            g = float(np.dot(rb, xb) / (np.dot(rb, rb) + 1e-12))
            item.update(clip=cid, lag=int(lag), q=q, q_win=qw, weak_align=bool(qw < ALIGN_Q_MIN),
                        feed=[int(fs), int(fe_)],
                        play=[int(lag), int(lag + len(ref))], ls_gain=g,
                        echo_dbfs=float(C.db(C.fe(x)[lag // C.HOP + 3:(lag + len(ref)) // C.HOP].mean())),
                        prompts_s=[round(lag / C.SR + p["at"], 3) for p in t.get("prompts", [])])
        if t["kind"] == "wearer":
            m = talker_mask(x, fl)
            item.update(active_frames=int(m.sum()),
                        level_dbfs=float(C.db(C.fe(x)[m].mean())) if m.any() else None)
        man["items"][t["id"]] = item
    man["floor"] = float(np.median(floors)) if floors else None
    man["weak_align"] = sorted(k for k, v in man["items"].items() if v.get("weak_align"))
    with open(os.path.join(out, "manifest.json"), "w") as f:
        json.dump(man, f, indent=1)
    return man

# ------------------------------------------------------------------- scoring


def echo_metrics(mic, out, play, floor, dump=None):
    """dB removed per band on echo-active frames; residual-over-floor
    fractions over the playback span; gate stats from the dump."""
    n = min(len(mic), len(out)) // C.HOP
    p0, p1 = play[0] // C.HOP, min(n, play[1] // C.HOP)
    ein = C.fe(mic)[:n]
    act = np.zeros(n, bool)
    act[p0 + 3:p1] = ein[p0 + 3:p1] > floor * 10
    m = {}
    for b in C.BANDS:
        a, o = C.fe(mic, b)[:n], C.fe(out, b)[:n]
        m["rem_" + b] = float(C.db(a[act].sum()) - C.db(o[act].sum()))
    eo = C.fe(out)[:n]
    span = slice(p0, p1)
    m["over10"] = float(np.mean(eo[span] > floor * 10))
    m["over20"] = float(np.mean(eo[span] > floor * 100))
    m["resid_dbfs"] = float(C.db(eo[act].mean()))
    # step 4's energy-VAD threshold for this set: the echo alone (almost)
    # never trips it
    m["vad_thr"] = float(max(floor * 10, np.percentile(eo[p0 + 3:p1], 95) * 2)) if p1 > p0 + 3 \
        else float("nan")
    m["echo_dbfs"] = float(C.db(ein[act].mean()))
    if dump is not None:
        k = (dump["off"] >= play[0]) & (dump["off"] < play[1])
        m["gate_rel"] = float(np.mean(dump["gate_rel"][k]))
        m["rearms"] = int(np.sum(np.diff(dump["onset"]) > 0))
    return m


TTP_WIN = 40      # frames (0.8 s): time-to-pass window after an onset (step 4)
FULL_WIN = 50     # frames (1 s): full-pass window


def nearend(talk_in, talk_out, echo_in, play, floor, va, mix_out=None, vad_thr=None):
    """Talker preservation from the shadow decomposition, on overlap frames
    (talker active over the floor and echo playing). Per onset (talker after
    >= 200 ms of silence, while the reply plays): time to pass = first frame
    of the AEC output (mix_out) over vad_thr, as step 4 (None = missed in
    0.8 s), and time to full pass = first talker frame kept within 6 dB
    (None = not in 1 s)."""
    n = min(len(talk_in), len(talk_out), len(va) * C.HOP) // C.HOP
    p0, p1 = play[0] // C.HOP, min(n, play[1] // C.HOP)
    va = va[:n] & (C.fe(talk_in)[:n] > floor * 10)
    ein = C.fe(echo_in)[:n]
    playing = np.zeros(n, bool)
    playing[p0 + 3:p1] = True
    ov = va & playing & (ein > floor * 10)
    m = {"ov_frames": int(ov.sum())}
    if ov.sum() < 5:
        return m
    for b in ("full", "lo", "mid", "hi"):
        a, o = C.fe(talk_in, b)[:n], C.fe(talk_out, b)[:n]
        m["kept_" + b] = float(C.db(o[ov].sum()) - C.db(a[ov].sum()))
    fk = C.db(C.fe(talk_out)[:n]) - C.db(C.fe(talk_in)[:n])
    m["kept_med"] = float(np.median(fk[ov]))
    m["crushed"] = float(np.mean(fk[ov] < -10))
    mo = C.fe(mix_out)[:n] if mix_out is not None else None
    ttp, tfull, k = [], [], 0
    while k < n:
        if va[k] and playing[k] and k >= 10 and not va[k - 10:k].any():
            j = next((q for q in range(k, min(n, k + FULL_WIN)) if va[q] and fk[q] > -6), None)
            tfull.append(None if j is None else (j - k) * 20)
            if mo is not None and vad_thr is not None and np.isfinite(vad_thr):
                j = next((q for q in range(k, min(n, k + TTP_WIN)) if mo[q] > vad_thr), None)
                ttp.append(None if j is None else (j - k) * 20)
            k += 10
        else:
            k += 1
    m["tfull_list"] = tfull
    if mo is not None and vad_thr is not None:
        m["ttp_list"] = ttp
        m["vad_thr_dbfs"] = float(C.db(vad_thr))
    return m


def _onset_median(lists, cap_ms):
    """Median over pooled onsets, a miss (None) counted as cap_ms; and the
    missed fraction."""
    v = [x for lst in lists for x in (lst or [])]
    if not v:
        return float("nan"), float("nan")
    return (float(np.median([cap_ms if x is None else x for x in v])),
            float(np.mean([x is None for x in v])))


def session_gain_scale(data, man=None):
    """The mic gain scale the device's AEC applied to a replay dir's captures:
    the manifest's mic_gain_scale when set, else from the session's effective
    mic gain (the gain actually applied: a saved gain() overrides
    start{gain=}), else its start{gain=} when that was left open (the
    saved-gain probe could not tell; \"1 or 0\")."""
    man = man or json.load(open(os.path.join(data, "manifest.json")))
    if man.get("mic_gain_scale") is not None:
        return float(man["mic_gain_scale"])
    p = os.path.join(os.path.dirname(os.path.abspath(data)), "session.json")
    if not os.path.exists(p) and man.get("session"):
        p = os.path.join(man["session"], "session.json")
    if not os.path.exists(p):
        return 1.0
    s = json.load(open(p))
    g = s.get("device", {}).get("gain_effective")
    if not isinstance(g, int):
        g = s.get("settings", {}).get("mic_gain", 1)
    return C.mic_gain_scale(g)


def build_jobs(data):
    man = json.load(open(os.path.join(data, "manifest.json")))
    it = man["items"]
    gs = session_gain_scale(data, man)
    # recordings whose reference did not align (prep: weak_align) are not scored
    echo = [k for k, v in it.items() if v["kind"] == "echo" and not v.get("weak_align")]
    talk = [k for k, v in it.items() if v["kind"] == "wearer" and v.get("active_frames", 0) >= 10]
    dts = [k for k, v in it.items() if v["kind"] == "dt" and not v.get("weak_align")]
    J = []
    for e in echo:
        J.append(dict(kind="echo", id=e, mic=os.path.join(data, e, "mic.wav"),
                      ref=os.path.join(data, e, "ref.wav"), feed=it[e]["feed"], play=it[e]["play"],
                      floor=it[e]["floor"]))
    # synthetic worn double talk: every echo capture with two wearer captures,
    # rolled so the phrases land at different points of the reply (incl. its onset)
    syn = os.path.join(data, "syn")
    os.makedirs(syn, exist_ok=True)
    for i, e in enumerate(echo):
        for j in range(min(2, len(talk))):
            tid = talk[(2 * i + j) % len(talk)]
            roll = (0.0, 1.5, 0.75)[(i + j) % 3]
            sid = f"{e}+{tid}@{roll}"
            d = os.path.join(syn, sid)
            if not os.path.exists(os.path.join(d, "mix.wav")):
                os.makedirs(d, exist_ok=True)
                ex = C.read_wav(os.path.join(data, e, "mic.wav"))
                tx = np.roll(C.read_wav(os.path.join(data, tid, "mic.wav")), -int(roll * C.SR))
                n = min(len(ex), len(tx))
                C.write_wav(os.path.join(d, "talk.wav"), tx[:n])
                C.write_wav(os.path.join(d, "mix.wav"), ex[:n] + tx[:n])
            J.append(dict(kind="syn", id=sid, level=it[tid].get("level"), echo_id=e,
                          mic=os.path.join(d, "mix.wav"), ref=os.path.join(data, e, "ref.wav"),
                          shadow=os.path.join(d, "talk.wav"), echo_mic=os.path.join(data, e, "mic.wav"),
                          feed=it[e]["feed"], play=it[e]["play"], floor=it[e]["floor"],
                          talk_floor=it[tid]["floor"]))
    # real wearer-over-echo, paired with an echo-only capture of the same clip
    for dt in dts:
        pair = [e for e in echo if it[e]["clip"] == it[dt]["clip"]]
        if not pair:
            continue
        e = pair[0]
        J.append(dict(kind="rdt", id=dt, mic=os.path.join(data, dt, "mic.wav"),
                      ref=os.path.join(data, dt, "ref.wav"), feed=it[dt]["feed"], play=it[dt]["play"],
                      floor=it[dt]["floor"], echo_mic=os.path.join(data, e, "mic.wav"),
                      echo_ref=os.path.join(data, e, "ref.wav"), echo_feed=it[e]["feed"],
                      echo_play=it[e]["play"], dlag=it[dt]["lag"] - it[e]["lag"],
                      prompts_s=it[dt].get("prompts_s", [])))
    for j in J:
        j["gain_scale"] = gs
    return J


def _run(args):
    job, params = args[:2]
    thr = args[2] if len(args) > 2 else {}
    if job["kind"] == "echo":
        r = run(job["mic"], job["ref"], params, feed=job["feed"], dump=True,
                gain_scale=job.get("gain_scale", 1.0))
        x = C.read_wav(job["mic"])
        m = echo_metrics(x, r["out"], job["play"], job["floor"], r["dump"])
        m["resid_over_floor"] = m["resid_dbfs"] - float(C.db(job["floor"]))
        return job["id"], m
    if job["kind"] == "syn":
        r = run(job["mic"], job["ref"], params, feed=job["feed"], shadow=job["shadow"], dump=True,
                gain_scale=job.get("gain_scale", 1.0))
        t = C.read_wav(job["shadow"])
        va = talker_mask(t, job["talk_floor"])
        m = nearend(t, r["shadow_out"], C.read_wav(job["echo_mic"]), job["play"], job["floor"], va,
                    mix_out=r["out"], vad_thr=thr.get(job.get("echo_id")))
        k = (r["dump"]["off"] >= job["play"][0]) & (r["dump"]["off"] < job["play"][1])
        m["gate_rel"] = float(np.mean(r["dump"]["gate_rel"][k]))
        m["level"] = job["level"]
        return job["id"], m
    # rdt: energy method (desk_aec.py "kept") vs the same clip's echo-only capture
    r = run(job["mic"], job["ref"], params, feed=job["feed"], gain_scale=job.get("gain_scale", 1.0))
    re_ = run(job["echo_mic"], job["echo_ref"], params, feed=job["echo_feed"],
               gain_scale=job.get("gain_scale", 1.0))
    di, do = C.read_wav(job["mic"]), r["out"]
    ei = C.shift(C.read_wav(job["echo_mic"]), -job["dlag"])
    eo = C.shift(re_["out"], -job["dlag"])
    n = min(len(di), len(ei), len(do), len(eo)) // C.HOP
    p0, p1 = job["play"][0] // C.HOP + 3, min(n, job["play"][1] // C.HOP)
    m = {}
    sel = np.zeros(n, bool)
    for b in ("full", "lo"):
        dI, eI = C.fe(di, b)[:n], C.fe(ei, b)[:n]
        dO, eO = C.fe(do, b)[:n], C.fe(eo, b)[:n]
        if b == "full":
            sel[p0:p1] = (dI[p0:p1] > 4 * eI[p0:p1]) & (eI[p0:p1] > job["floor"] * 10)
        if sel.sum() >= 5:
            v_in = np.maximum(dI - eI, 1e-15)
            v_out = np.maximum(dO - eO, 1e-15)
            m["kept_" + b] = float(C.db(v_out[sel].sum()) - C.db(v_in[sel].sum()))
    m["ov_frames"] = int(sel.sum())
    return job["id"], m


_JOBS = {}
_POOL = [None]


def evaluate(data, params):
    """Score one set on a replay dir. params: a full tune table or C.REF_SET."""
    if data not in _JOBS:
        _JOBS[data] = build_jobs(data)
    jobs = _JOBS[data]
    tools()
    tree_defaults()
    if _POOL[0] is None:
        _POOL[0] = ProcessPoolExecutor(min(8, os.cpu_count() or 4))
    # echo first: the syn jobs' time to pass uses the same set's echo-only
    # residual (VAD threshold, as step 4)
    per = dict(_POOL[0].map(_run, [(j, params) for j in jobs if j["kind"] == "echo"]))
    thr = {i: m.get("vad_thr") for i, m in per.items()}
    per.update(_POOL[0].map(_run, [(j, params, thr) for j in jobs if j["kind"] != "echo"]))
    return summarize(jobs, per), per


def _mean(v):
    v = [x for x in v if x is not None and np.isfinite(x)]
    return float(np.mean(v)) if v else float("nan")


def summarize(jobs, per):
    kinds = {j["id"]: j["kind"] for j in jobs}
    E = [per[i] for i, k in kinds.items() if k == "echo"]
    Sy = [per[i] for i, k in kinds.items() if k == "syn"]
    R = [per[i] for i, k in kinds.items() if k == "rdt"]
    S = {"n_echo": len(E), "n_syn": len(Sy), "n_rdt": len(R)}
    for key in ("rem_full", "rem_lo", "rem_mid", "rem_hi", "over10", "over20", "gate_rel",
                "rearms", "resid_dbfs", "echo_dbfs", "resid_over_floor"):
        S[key] = _mean([m.get(key) for m in E])
    for key in ("kept_full", "kept_lo", "kept_hi", "kept_med", "crushed", "gate_rel", "vad_thr_dbfs"):
        S["syn_" + key] = _mean([m.get(key) for m in Sy])
    # time to pass (step 4's method; miss = 0.8 s) and to full pass (gate release; miss = 1 s)
    S["syn_ttfp_ms"], S["syn_ttp_missed"] = _onset_median([m.get("ttp_list") for m in Sy], TTP_WIN * 20)
    S["syn_tfull_ms"], S["syn_tfull_missed"] = _onset_median([m.get("tfull_list") for m in Sy],
                                                             FULL_WIN * 20)
    for lvl in ("normal", "quiet", "loud"):
        S[f"syn_kept_{lvl}"] = _mean([m.get("kept_full") for m in Sy if m.get("level") == lvl])
    for key in ("kept_full", "kept_lo"):
        S["rdt_" + key] = _mean([m.get(key) for m in R])
    S["echo"], S["kept"], S["crushed"], S["ttfp"] = (S["rem_full"], S["syn_kept_full"],
                                                     S["syn_crushed"], S["syn_ttfp_ms"])
    return S

# -------------------------------------------------------------- guard


def guard_sessions(sess_dir, given=None, roles=False):
    """Other sittings, the guard against fitting one sitting: the given dirs,
    else the other finished non-sim sessions next to this one. By default
    only those of the same device (BLE name) guard; when there are none, the
    other units' sittings do, as a secondary guard. With roles=True returns
    [(dir, "same device" | "other unit")], else the dirs."""
    me = C.Session(sess_dir).s.get("settings", {}).get("name") if os.path.exists(
        os.path.join(sess_dir, "session.json")) else None
    if given is not None:
        cands = given
    else:
        parent = os.path.dirname(os.path.abspath(sess_dir))
        cands = [os.path.join(parent, d) for d in sorted(os.listdir(parent))]
    out = []
    for d in cands:
        if os.path.abspath(d) == os.path.abspath(sess_dir):
            continue
        p = os.path.join(d, "session.json")
        if not os.path.exists(p):
            continue
        s = json.load(open(p))
        if "sim" in s.get("mode", ""):
            continue
        n_echo = sum(1 for t in s.get("plan", []) if t["kind"] == "echo" and not t.get("aec")
                     and t.get("status") == "done")
        n_talk = sum(1 for t in s.get("plan", []) if t["kind"] == "wearer" and t.get("status") == "done")
        if n_echo and n_talk:
            same = me is not None and s.get("settings", {}).get("name") == me
            out.append((d, "same device" if same else "other unit"))
    if given is None and any(r == "same device" for _, r in out):
        out = [x for x in out if x[1] == "same device"]
    out.sort(key=lambda x: x[1] != "same device")
    return out if roles else [d for d, _ in out]


_GUARD_CACHE = {}


def guard_scores(guards, params):
    """{guard sitting: {echo, kept, crushed, ttfp}} of one set (cached: the
    replay is deterministic)."""
    out = {}
    for g in guards:
        key = (os.path.abspath(g), json.dumps(params, sort_keys=True))
        if key not in _GUARD_CACHE:
            data = os.path.join(g, "replay")
            if not os.path.exists(os.path.join(data, "manifest.json")):
                prep(g)
            S, _ = evaluate(data, params)
            _GUARD_CACHE[key] = {k: S[k] for k in ("echo", "kept", "crushed", "ttfp")}
        out[g] = _GUARD_CACHE[key]
    return out


# guard limits: (metric, slack, sign: +1 = higher is better, near-end metric)
GUARD_LIMITS = (("echo", 1.0, 1, False), ("crushed", 0.03, -1, True),
                ("kept", 0.3, 1, True), ("ttfp", 15, -1, True))
# the near-end ones: a recommendation never loses to the current defaults on
# these beyond the slack, on the session itself or on any guard sitting
NE_LIMITS = tuple((k, slack, sign) for k, slack, sign, ne in GUARD_LIMITS if ne)
# ... nor on echo removed beyond this (dB): the guard vs a family's start
# alone lets a set from a weaker start (A15) lose echo to the defaults
ECHO_SLACK = next(slack for k, slack, _, _ in GUARD_LIMITS if k == "echo")


def _fin(*v):
    return all(x is not None and np.isfinite(x) for x in v)


def near_end_loss(x, b):
    """The near-end metrics (crushed, kept, ttfp) on which scores x lose to
    b beyond the guard slack. Only metrics present on both sides count."""
    return [k for k, slack, sign in NE_LIMITS
            if _fin(x.get(k), b.get(k)) and (x[k] - b[k]) * sign < -slack]


def echo_loss(x, b):
    """["echo"] if scores x remove more than ECHO_SLACK dB less echo than b
    (both scored), else []."""
    return ["echo"] if _fin(x.get("echo"), b.get("echo")) and x["echo"] - b["echo"] < -ECHO_SLACK else []


def vs_current(S, anchor, gx, gc, roles):
    """A set against the current defaults: it must not lose on the near end
    (crushed, kept, ttfp, guard slack) nor remove more than ECHO_SLACK dB
    less echo, on this session (S vs anchor) and on every guard sitting (gx
    vs gc). Returns (ok, log lines); a failing line names the lost checks."""
    def vals(x, c):
        return (f" (echo {x['echo']:.1f} kept {x['kept']:.2f} crushed {x['crushed']:.3f} ttfp "
                f"{x['ttfp']:.0f} vs {c['echo']:.1f}/{c['kept']:.2f}/{c['crushed']:.3f}/{c['ttfp']:.0f})")
    lines, ok = [], True
    bad = echo_loss(S, anchor) + near_end_loss(S, anchor)
    if bad:
        ok = False
        lines.append("on this sitting vs current: LOSES " + ",".join(bad) + vals(S, anchor))
    for g in gx:
        x, c = gx[g], gc[g]
        bad = echo_loss(x, c) + near_end_loss(x, c)
        if bad:
            ok = False
            lines.append(f"on {os.path.basename(g)} ({roles.get(g, 'guard')}) vs current: LOSES "
                         + ",".join(bad) + vals(x, c))
    if ok:
        lines.append(f"echo and near end vs current: ok on this sitting and {len(gx)} guard sitting(s)")
    return ok, lines


def guard_check(x, b):
    """One guard sitting: candidate scores x vs the starting set's b. Only the
    metrics that exist on both sides are checked (desk and other-unit
    sittings often have no near-end score). Returns (ok, checks run, near-end
    checks run, failed metrics)."""
    n = n_ne = 0
    bad = []
    for k, slack, sign, ne in GUARD_LIMITS:
        a, c = x.get(k), b.get(k)
        if a is None or c is None or not (np.isfinite(a) and np.isfinite(c)):
            continue
        n += 1
        n_ne += ne
        if (a - c) * sign < -slack:
            bad.append(k)
    return not bad, n, n_ne, bad


def guard_verdict(cand_J, base_J, gx, gb, roles, no_guard_margin=1.0):
    """A ranked candidate against every guard sitting. It must not lose on
    any check that ran, and if no near-end check ran anywhere the guard is
    vacuous for the near end: the candidate then needs the no-guard margin.
    Returns (adopt, log lines, checks run, near-end checks run)."""
    ok, n_all, ne_all, lines = True, 0, 0, []
    for g in gx:
        okg, n, n_ne, bad = guard_check(gx[g], gb[g])
        ok &= okg
        n_all += n
        ne_all += n_ne
        x, b = gx[g], gb[g]
        lines.append(f"on {os.path.basename(g)} ({roles.get(g, 'guard')}): echo {x['echo']:.1f} crushed "
                     f"{x['crushed']:.3f} kept {x['kept']:.2f} ttfp {x['ttfp']:.0f} vs "
                     f"{b['echo']:.1f}/{b['crushed']:.3f}/{b['kept']:.2f}/{b['ttfp']:.0f}, "
                     f"{n}/{len(GUARD_LIMITS)} checks ({n_ne} near end) "
                     + ("ok" if okg else "LOSES " + ",".join(bad)))
    if ok and ne_all == 0 and cand_J <= base_J + no_guard_margin:
        ok = False
        lines.append(f"no near-end guard check ran: needs the {no_guard_margin} dB no-guard margin")
    lines.append(f"{n_all} guard checks ran ({ne_all} near end)")
    return ok, lines, n_all, ne_all

# -------------------------------------------------------------------- search

# narrowed space around the starting sets: (low, high, integer)
SPACE = {
    "gate_kappa": (0.25, 0.75, False),
    "sup_floor": (0.10, 0.30, False),      # never below the old default
    "sup_beta": (1.0, 2.0, False),
    "gate_hang_ms": (600, 1800, True),
    "gate_absfloor": (0.5, 1.5, False),
    "gate_edge_abs": (0.6, 3.0, False),
    "steady_gcap": (0.1, 0.5, False),
    "cap_lo_gcap": (0.25, 1.0, False),     # only with cap_split_hz > 0
    "cap_hi_gcap": (0.05, 0.5, False),     # only with cap_hi_split_hz > 0
    "cap_hi_split_hz": (1000, 3000, True), # only when on; kept above cap_split_hz
}
# the top cap band, switched on from a set without it (or off from one with it)
HI_ON = ((1600, 0.1), (1600, 0.15))


# past the near-end slack (NE_LIMITS) the objective drops this many dB per
# slack unit: steep enough that no echo gain pays for it (the search still
# walks back towards the bound), and such a set is never recommended
HARD_DB_PER_SLACK = 25.0


def objective(S, anchor):
    """Echo removed (dB), penalized for near-end loss beyond the current
    defaults (anchor): crushed +0.02, kept -0.5 dB, ttfp +20 ms are free;
    past that 50 dB per unit crushed, 2 dB per dB kept, 0.05 dB per ms.
    Past the guard slack (crushed +0.03, kept -0.3 dB, ttfp +15 ms) a set is
    infeasible (feasible() is False) and loses 25 dB per slack unit on top,
    so echo gains never buy a worse talker than the defaults."""
    j = S["echo"]
    if np.isfinite(S["crushed"]) and np.isfinite(anchor["crushed"]):
        j -= 50 * max(0.0, S["crushed"] - (anchor["crushed"] + 0.02))
    if np.isfinite(S["kept"]) and np.isfinite(anchor["kept"]):
        j -= 2 * max(0.0, (anchor["kept"] - 0.5) - S["kept"])
    if np.isfinite(S["ttfp"]) and np.isfinite(anchor["ttfp"]):
        j -= 0.05 * max(0.0, S["ttfp"] - (anchor["ttfp"] + 20))
    for k, slack, sign in NE_LIMITS:
        if _fin(S.get(k), anchor.get(k)):
            j -= HARD_DB_PER_SLACK * max(0.0, (anchor[k] - S[k]) * sign - slack) / slack
    return float(j)


def feasible(S, anchor):
    """No near-end loss to the current defaults beyond the guard slack."""
    return not near_end_loss(S, anchor)


def _clean(k, v, it):
    if k.endswith("_ms"):
        return int(round(v / 20.0)) * 20
    if k.endswith("_hz"):
        return int(round(v / 50.0)) * 50
    return int(round(v)) if it else float(round(v, 3))


def search(data, fam, base, deadline, log, rows, anchor, guards, roles=None, current=None):
    """Start at the family's set, try the top cap band on/off, grid (kappa x
    floor), then coordinate refinement until the deadline. A searched set
    replaces the starting set only if it beats it by > 0.5 dB of objective,
    does not lose on any guard sitting vs the starting set (echo -1 dB,
    crushed +0.03, kept -0.3 dB, ttfp +15 ms) AND does not lose on the near
    end (crushed, kept, ttfp) or echo (same slack) to the current defaults
    (`current`, scores `anchor`) on this sitting or on any guard sitting.
    With no guard sitting the margin is 1.0 dB. One sitting is one fit and
    ~16 phrases: these rules are there against overfitting it."""
    Sb, _ = evaluate(data, base)
    jb = objective(Sb, anchor)
    rows.append(dict(family=fam, tag="base", params=base, S=Sb, J=jb, feasible=feasible(Sb, anchor)))
    log(f"  family {fam}: start echo {Sb['echo']:.1f} dB, kept {Sb['kept']:.2f}, "
        f"crushed {Sb['crushed']:.3f}, ttfp {Sb['ttfp']:.0f} ms  J {jb:.2f}")
    if not np.isfinite(Sb["kept"]):
        log("  no wearer-only captures: near-end is unscored, so no search")
        return dict(params=base, S=Sb, J=jb, base_J=jb, best_J=jb, searched=0, adopted=False)
    best = (jb, base, Sb)
    seen = {json.dumps(base, sort_keys=True)}
    n = 0

    def tryp(p, tag):
        nonlocal best, n
        k = json.dumps(p, sort_keys=True)
        if k in seen or time.time() > deadline:
            return None
        seen.add(k)
        S, _ = evaluate(data, p)
        j = objective(S, anchor)
        n += 1
        rows.append(dict(family=fam, tag=tag, params=p, S=S, J=j, feasible=feasible(S, anchor)))
        if j > best[0] + 0.02:
            best = (j, p, S)
            log(f"    + {tag}: echo {S['echo']:.1f} kept {S['kept']:.2f} crushed {S['crushed']:.3f} "
                f"ttfp {S['ttfp']:.0f}  J {j:.2f}")
        return j

    # cap depth first (the deadline may cut the grid short): the top cap
    # band on at two depths, or off if the starting set has it
    if "cap_hi_split_hz" in base:
        cur = best[1]
        if cur["cap_hi_split_hz"]:
            tryp(dict(cur, cap_hi_split_hz=0), "top band off")
        else:
            for hz, g in HI_ON:
                if not cur.get("cap_split_hz") or hz > cur["cap_split_hz"]:
                    tryp(dict(cur, cap_hi_split_hz=hz, cap_hi_gcap=g),
                         f"top band {hz} Hz / {g}")
    # the grid around the best set so far (the top band step may have moved it)
    g0 = best[1]
    k0 = g0["gate_kappa"]
    for kf in (1.0, 0.85, 1.15, 0.7, 1.3):
        for fl in (0.15, 0.10, 0.20, 0.25):
            p = dict(g0, gate_kappa=round(min(0.75, k0 * kf), 3), sup_floor=fl)
            tryp(p, f"grid kappa={p['gate_kappa']} floor={fl}")
    for rd in range(3):
        improved = False
        for key, (lo, hi, it) in SPACE.items():
            if key not in best[1]:
                continue
            if key == "cap_lo_gcap" and not best[1].get("cap_split_hz"):
                continue
            if key in ("cap_hi_gcap", "cap_hi_split_hz") and not best[1].get("cap_hi_split_hz"):
                continue
            cur = best[1]
            v0 = cur[key]
            for f in (0.8, 1.25):
                v = _clean(key, min(hi, max(lo, v0 * f)), it)
                if C.same(v, v0):
                    continue
                if key == "cap_hi_split_hz" and cur.get("cap_split_hz") and v <= cur["cap_split_hz"]:
                    continue
                jb_before = best[0]
                tryp(dict(cur, **{key: v}), f"r{rd} {key}={v}")
                improved |= best[0] > jb_before
        if not improved or time.time() > deadline:
            break
    j, p, S = best
    margin = 0.5 if guards else 1.0
    # only sets that keep the near end within the slack of the current
    # defaults on this sitting (feasible) and do not remove > ECHO_SLACK dB
    # less echo than them there are candidates at all
    over = [r for r in rows if r["family"] == fam and r["tag"] != "base" and r["J"] > jb + margin]
    ranked = sorted([r for r in over if r["feasible"] and not echo_loss(r["S"], anchor)],
                    key=lambda r: -r["J"])
    take, guard_log, checks = None, [], []
    n_inf = sum(1 for r in over if not r["feasible"])
    n_echo = sum(1 for r in over if r["feasible"] and echo_loss(r["S"], anchor))
    if n_inf:
        guard_log.append(f"{n_inf} set(s) over the margin lose near end to the current defaults on "
                         "this sitting: not candidates")
    if n_echo:
        guard_log.append(f"{n_echo} set(s) over the margin lose echo (> {ECHO_SLACK:g} dB) to the "
                         "current defaults on this sitting: not candidates")
    if ranked and guards:
        gb = guard_scores(guards, base)
        gc = guard_scores(guards, current) if current is not None else gb
        for r in ranked[:3]:
            gx = guard_scores(guards, r["params"])
            ok, lines, n_all, ne_all = guard_verdict(r["J"], jb, gx, gb, roles or {})
            if current is not None and C.diff(base, current):
                okc, lc = vs_current(r["S"], anchor, gx, gc, roles or {})
                ok &= okc
                lines += lc
            checks.append(dict(tag=r["tag"], checks=n_all, near_end=ne_all, adopted=ok))
            guard_log += [f"{r['tag']} {ln}" for ln in lines]
            guard_log.append(f"{r['tag']}: " + ("adopted" if ok else "rejected"))
            if ok:
                take = r
                break
    elif ranked:
        take = ranked[0]
        guard_log.append(f"no guard sitting: adopted on a {margin} dB margin")
    for g in guard_log:
        log("    " + g)
    log(f"  family {fam}: {n} candidates; best J {j:.2f} vs start {jb:.2f} -> "
        + (f"searched set ({take['tag']})" if take else "keep the starting set"))
    if take:
        return dict(params=take["params"], S=take["S"], J=take["J"], base_J=jb, searched=n,
                    best_J=j, best_params=p, guard=guard_log, guard_checks=checks, adopted=True)
    return dict(params=base, S=Sb, J=jb, base_J=jb, searched=n, best_J=j, best_params=p,
                guard=guard_log, guard_checks=checks, adopted=False)

# --------------------------------------------------------------------- step 3


def step3(sess_dir, budget=90.0, log=print, guards=None):
    """guards: None = auto (other sittings next to this one), [] = none."""
    S = C.Session(sess_dir)
    man = prep(sess_dir)
    data = os.path.join(sess_dir, "replay")
    weak = man.get("weak_align") or []
    if weak:
        log(f"  !! WARNING: the reference did not align in {len(weak)} recording(s) (xcorr q_win < "
            f"{ALIGN_Q_MIN:g}); they are NOT scored: "
            + ", ".join(f"{k} (q_win {man['items'][k]['q_win']:.1f})" for k in weak))
    nE = sum(1 for v in man["items"].values() if v["kind"] == "echo" and not v.get("weak_align"))
    if nE == 0:
        raise SystemExit("no usable AEC-off echo-only captures in this session (none recorded, or "
                         "none aligned): nothing to optimise")
    t0 = time.time()
    tree = tree_defaults()
    dev_def = S.defaults()
    dev_keys = sorted(dev_def) if dev_def else None
    # the firmware's defaults, as a table this tree's replay understands
    defaults = C.firmware_table(tree, dev_def) if dev_def else dict(tree)
    live = C.firmware_table(tree, S.live()) if S.live() else None
    unknown = C.missing(dev_def, tree)
    if unknown:
        log(f"  note: the device has keys this tree's AEC lacks (ignored offline): {unknown}")
    if dev_def and C.diff(defaults, tree):
        log(f"  note: the device defaults differ from this tree's: {C.diff(defaults, tree)}")
    sets = {}
    has_ref = CB.aec_replay_ref() is not None
    current_note = None
    if dev_def is not None:
        sets["current"] = defaults
    elif has_ref:
        current_note = "firmware without aec_tune: 'current' is scored as the 0.8.17 source"
        sets["current"] = C.REF_SET
    else:
        # no 0.8.17 tag in this clone: the closest this tree can replay is the
        # old-gate set (the 0.8.17 suppressor parameters, but this tree's
        # onset and gate fixes, so it flatters the firmware)
        current_note = (f"firmware without aec_tune and no {CB.REF_TAG} tag in this clone: "
                        "'current' is scored as this tree with the old-gate set, an approximation "
                        f"(it has the onset fixes the firmware lacks); `git fetch --tags` for the "
                        f"{CB.REF_TAG} reference")
        sets["current"] = C.apply(defaults, C.NAMED["old-gate"])
    # what the device ran: firmware older than this tree's gain-1 convention
    # applied some gate keys in mic units, so at gain != 1 its sets replay
    # with them divided by the gain scale (the replay multiplies them back)
    gs = session_gain_scale(data)
    mu = C.mic_unit_keys(S.s.get("device", {})) if dev_def is not None and not C.same(gs, 1.0) else []
    if mu:
        sets["current"] = C.to_gain1(sets["current"], mu, gs)
        live = C.to_gain1(live, mu, gs) if live else live
        mnote = (f"firmware {S.s.get('device', {}).get('fw', '?')} ran {', '.join(mu)} in mic units at "
                 f"mic gain scale {gs:g}: 'current' (and 'live') replay them / {gs:g} in this tree's "
                 f"gain-1 units ({', '.join(f'{k} {sets['current'][k]:.4g}' for k in mu)}), as the "
                 "device ran them; the other sets are gain-1 values for this tree's firmware")
        current_note = f"{current_note}; {mnote}" if current_note else mnote
    if current_note:
        log("  " + current_note)
    if live and C.diff(live, sets["current"] if mu else defaults):
        sets["live"] = live
        log(f"  note: the device ran a non-default tune at step 1: {C.diff(live, defaults)}")
    if has_ref:
        sets[C.REF_SET] = C.REF_SET
    for k, v in C.NAMED.items():
        sets[k] = C.apply(defaults, v)
    res = {"sets": {}, "families": {}, "device_keys": dev_keys, "defaults": defaults,
           "current_note": current_note, "weak_align": weak}
    for name, p in sets.items():
        Sx, per = evaluate(data, p)
        res["sets"][name] = dict(params=p, S=Sx, per=per,
                                 same_as_current=(dev_def is not None and p != C.REF_SET
                                                  and not C.diff(p, defaults)),
                                 applicable=(p != C.REF_SET and dev_keys is not None
                                             and not C.missing(C.diff(p, defaults), dev_keys)))
        log(f"  {name:8s} echo {Sx['echo']:5.1f} dB (hi {Sx['rem_hi']:4.1f})  over20 {Sx['over20']:.2f}  "
            f"gate {Sx['gate_rel']:.2f}  kept {Sx['kept']:5.2f}  crushed {Sx['crushed']:.3f}  "
            f"ttfp {Sx['ttfp']:4.0f} ms  real-dt kept {Sx['rdt_kept_full']:5.2f}")
    rows = []
    anchor = res["sets"]["current"]["S"]
    j_cur = objective(anchor, anchor)
    res["current_J"] = j_cur
    gr = guard_sessions(sess_dir, guards, roles=True) if guards != [] else []
    gl = [g for g, _ in gr]
    roles = dict(gr)
    res["guards"] = gl
    res["guard_roles"] = roles
    log(f"  guard sittings: {len(gl)}" + "".join(f"\n    {g} ({r})" for g, r in gr))
    if gr and all(r == "other unit" for _, r in gr) and guards is None:
        log("  note: no other sitting of this device: other units' sittings guard (secondary)")
    fams = {"current": defaults, "A15": C.apply(defaults, C.NAMED["A15"])}
    if dev_def is None:
        fams = {"A15": C.apply(defaults, C.NAMED["A15"])}
    remaining = budget - (time.time() - t0)
    for i, (fam, base) in enumerate(fams.items()):
        share = remaining * (0.6 if i == 0 and len(fams) > 1 else 1.0)
        res["families"][fam] = search(data, fam, base, time.time() + max(5.0, share), log, rows,
                                      anchor, gl, roles, current=sets["current"]
                                      if sets["current"] != C.REF_SET else None)
        remaining = budget - (time.time() - t0)
    with open(os.path.join(sess_dir, "results", "step3_candidates.jsonl"), "w") as f:
        for r in rows:
            f.write(json.dumps(r) + "\n")
    # final choice among {current defaults, family results}. Every set other
    # than the defaults must not lose near end or echo (> ECHO_SLACK) to them,
    # on this sitting or on any guard sitting (a family's unsearched start,
    # e.g. A15, was never checked against them)
    cands = []
    if dev_def is not None:
        cands.append(("current", defaults, anchor, j_cur))
    cur_p = sets["current"]
    gc = guard_scores(gl, cur_p) if gl else {}
    res["rejected_vs_current"] = []
    for f, r in res["families"].items():
        if not C.diff(r["params"], defaults):
            cands.append((f"family {f}", r["params"], r["S"], r["J"]))
            continue
        ok, lines = vs_current(r["S"], anchor, guard_scores(gl, r["params"]) if gl else {}, gc, roles)
        for ln in lines:
            log(f"  family {f} ({'searched set' if r.get('adopted') else 'start'}) {ln}")
        if ok:
            cands.append((f"family {f}", r["params"], r["S"], r["J"]))
        else:
            res["rejected_vs_current"].append(dict(source=f"family {f}", params=r["params"], S=r["S"],
                                                   J=r["J"], why=lines))
            log(f"  family {f}: not a recommendation (loses to the current defaults)")
    if dev_def is None and not cands:
        # firmware without aec_tune and no family set holds the near end:
        # keep the firmware as it is (nothing to apply)
        cands.append(("current", defaults, anchor, j_cur))

    def pick(cs):
        cs = sorted(cs, key=lambda c: -c[3])
        top = cs[0]
        # a change must earn its keep: within 0.3 dB, prefer the current defaults
        if top[0] != "current" and any(c[0] == "current" and c[3] > top[3] - 0.3 for c in cs):
            top = next(c for c in cs if c[0] == "current")
        if top[0] != "current" and not C.diff(top[1], defaults):
            top = ("current",) + tuple(top[1:])
        return top
    rec = pick(cands)
    if dev_def is not None and not np.isfinite(anchor["kept"]) and rec[0] != "current":
        # no wearer captures: echo alone must not pick a set (it would trade
        # away the near end unseen); step 4 still tests the best alternative
        log("  no wearer captures, so near end is unscored: keeping the current defaults")
        rec = cands[0]
    name, p, Sx, j = rec
    res["recommended"] = dict(source=name, params=p, S=Sx, J=j, keep_current=(name == "current"),
                              lua=C.lua_tune_line(p, defaults), defines=C.c_defines(p, defaults),
                              tune=C.diff(p, defaults),
                              applicable=dev_keys is not None and not C.missing(C.diff(p, defaults), dev_keys))
    # what step 4 tests against the defaults: the recommendation, or, when
    # that is "keep current", the best alternative (so the on-device A/B
    # still checks the offline verdict)
    # (sets rejected against the defaults are still worth checking there)
    alts = sorted([c for c in cands if c[0] != "current" and C.diff(c[1], defaults)]
                  + [(r["source"], r["params"], r["S"], r["J"]) for r in res["rejected_vs_current"]],
                  key=lambda c: -c[3])
    if name != "current":
        res["step4_candidate"] = dict(source=name, params=p, alternative=False)
    elif alts:
        res["step4_candidate"] = dict(source=alts[0][0], params=alts[0][1], alternative=True)
    else:
        res["step4_candidate"] = dict(source="B15", params=C.apply(defaults, C.NAMED["B15"]),
                                      alternative=True)
    out = os.path.join(sess_dir, "results", "step3.json")
    tmp = out + ".part"
    json.dump(res, open(tmp, "w"), indent=1, default=float)
    os.replace(tmp, out)
    log(f"  recommended ({name}, J {j:.2f}, current {j_cur:.2f}): {res['recommended']['lua']}")
    log(f"  step 3 took {time.time() - t0:.0f} s")
    return res

# ----------------------------------------------------------------------- CLI


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return
    cmd = a[0]
    if cmd == "prep":
        man = prep(a[1])
        for k, v in man["items"].items():
            print(k, {x: (round(y, 4) if isinstance(y, float) else y) for x, y in v.items()
                      if x in ("kind", "lag", "q", "ls_gain", "echo_dbfs", "level_dbfs")})
    elif cmd == "step3":
        budget = float(a[a.index("--budget") + 1]) if "--budget" in a else 90.0
        guards = None
        if "--no-guard" in a:
            guards = []
        elif "--guard" in a:
            guards = [x for x in a[a.index("--guard") + 1:] if not x.startswith("--")]
        step3(a[1], budget, guards=guards)
    elif cmd == "eval":
        prep(a[1])
        S0 = C.Session(a[1])
        base = C.firmware_table(tree_defaults(), S0.defaults() or {})
        p = C.REF_SET if "--ref" in a else dict(base)
        if p != C.REF_SET:
            for kv in a[2:]:
                if not kv.startswith("--"):
                    k, v = kv.split("=")
                    p[k] = float(v)
        S, per = evaluate(os.path.join(a[1], "replay"), p)
        print(json.dumps(S, indent=1))
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
