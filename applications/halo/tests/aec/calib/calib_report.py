#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10,<3.13"
# dependencies = ["numpy", "lc3py"]
# ///
"""Step 4 analysis and the session report (report.md).

  calib_report.py SESSION [--silero]

Step 4 (AEC on, device defaults 'cur' vs recommended 'rec', same clip):
  echo removed    AEC-off echo-only (step 2a, same clip) vs AEC-on echo-only,
                  per band, on echo-active frames (as desk_aec.py)
  resid > floor   AEC-on residual over the step 1 noise floor; over10/over20 =
                  fraction of playback frames > floor + 10/20 dB (VAD proxy)
  VAD threshold   per set: max(floor + 10 dB, p95 of that set's echo-only
                  residual + 3 dB): an energy VAD that the echo alone
                  (almost) never trips
  wearer onset    from diag('stats').p_mic (raw mic power, 50 ms EMA, sampled
                  every ~60 ms): first sample after the cue where the dt pass
                  exceeds the echo-only pass at the same reply time by 4 dB
  time to pass    onset -> first 20 ms output frame over the VAD threshold
  pass fraction   frames over the threshold in the first 0.8 s of the phrase
  kept            output (minus the echo-only residual) over the phrase vs the
                  raw wearer power from p_mic (full band vs voice band: biased,
                  read the A/B difference)
"""
import json
import os
import subprocess
import sys
import warnings

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import calib_common as C  # noqa: E402


def _col(rec, name):
    f = rec.get("diag_fields") or []
    if name not in f or not rec.get("diag"):
        return None
    i = f.index(name)
    return np.array([r[i] for r in rec["diag"]], float)


def _f(v, fmt="{:.1f}"):
    try:
        return "–" if v is None or not np.isfinite(v) else fmt.format(v)
    except (TypeError, ValueError):
        return str(v)

# -------------------------------------------------------------------- step 4


def _ref_for(S, cid):
    p = os.path.join(S.root, "replay", f"ref_spk_{cid}.wav")
    if os.path.exists(p):
        return C.read_wav(p)
    import calib_offline as O
    st = S.s["settings"]
    lc3 = open(os.path.join(S.root, "clips", f"{cid}.lc3"), "rb").read()
    os.makedirs(os.path.join(S.root, "replay"), exist_ok=True)
    return O.spk_ref(C.lc3_decode(lc3), st["volume"], st["spk_gain"], st["budget"],
                     os.path.join(S.root, "replay"), cid)


def _hint(rec):
    return rec["diag"][0][0] / C.LC3_BPS if rec.get("diag") else None


def align_all(S):
    """Lag (samples, capture timeline) of the clip in every clip trial. AEC-off:
    windowed xcorr (pre-cue part for dt). AEC-on: xcorr near the expected lag
    (playback-command time + the session's median AEC-off offset + the AEC
    delay), falling back to the expectation."""
    import calib_offline as O
    recs, lags, offs = {}, {}, []
    for t in S.trials(done=True):
        if t["clip"] is None:
            continue
        r = S.load_trial(t["id"])
        recs[t["id"]] = r
        if t["aec"]:
            continue
        ref = _ref_for(S, t["clip"])
        xc = r["x"].copy()
        h = _hint(r)
        if t["kind"] == "dt":
            xc[int(((h or 1.0) + min(p["at"] for p in t["prompts"]) + 0.15) * C.SR):] = 0
        lag = O.windowed_lag(xc, ref, h - 0.05, h + 0.6)[0] if h is not None else C.xcorr_lag(xc, ref)[0]
        lags[t["id"]] = lag
        if h is not None:
            offs.append(lag - h * C.SR)
    off = float(np.median(offs)) if offs else 0.03 * C.SR
    for t in S.trials(done=True):
        if t["clip"] is None or not t["aec"]:
            continue
        r = recs[t["id"]]
        ref = _ref_for(S, t["clip"])
        h = _hint(r)
        xc = r["x"].copy()
        if t["kind"] == "dt":
            xc[int(((h or 1.0) + min(p["at"] for p in t["prompts"]) + 0.15) * C.SR):] = 0
        if h is not None:
            exp = h * C.SR + off + C.AEC_DELAY
            lag, q = O.windowed_lag(xc, ref, exp / C.SR - 0.03, exp / C.SR + 0.03)
            lags[t["id"]] = lag if q > 8 else int(exp)
        else:
            lags[t["id"]] = C.xcorr_lag(xc, ref)[0]
    return recs, lags


def step4_trials(S):
    """Step 4 recordings made with the sets session.json now names
    (step4_tune), judged by the aec_tune() table read back before each
    recording. A recording of an earlier 'rec' (step 3 rerun since) or one
    whose readback differs is left out. Returns (kept, [(id, why)])."""
    tk = S.s.get("step4_tune") or {}
    dflt = S.defaults()
    keep, excl = [], []
    for t in S.trials(step="4", done=True):
        try:
            got = json.load(open(S.trial_path(t["id"], "json"))).get("tune_table")
        except (OSError, ValueError):
            got = None
        want = tk.get(t.get("tune"))
        if want is None or got is None or dflt is None:
            excl.append((t["id"], "no aec_tune record"))
            continue
        bad = C.tune_mismatch(C.apply(dflt, want), got)
        if bad:
            excl.append((t["id"], "recorded with another set: " + ", ".join(
                f"{k} {C.fmt_val(v) if v is not None else '?'} (now {C.fmt_val(w)})"
                for k, (w, v) in sorted(bad.items()))))
            continue
        keep.append(t)
    return keep, excl


def _weak_align(S):
    p = os.path.join(S.root, "replay", "manifest.json")
    return set(json.load(open(p)).get("weak_align") or []) if os.path.exists(p) else set()


def step4(S):
    warnings.filterwarnings("ignore", "Mean of empty slice")
    ts, excluded = step4_trials(S)
    if not ts:
        return {"sets": {}, "utterances": [], "excluded": excluded} if excluded else None
    weak = _weak_align(S)
    recs, lags = align_all(S)
    fl_db = S.s.get("floor_dbfs")
    if fl_db is None:
        fl_db = float(np.median([C.db(np.median(C.fe(recs[k]["x"])[10:45])) for k in recs]))
    fl = 10 ** (fl_db / 10)
    clips = S.s["clips"]
    out = {"floor_dbfs": fl_db, "sets": {}, "utterances": [], "excluded": excluded}

    def tl(tid, n):
        """frame energies per band on the reply timeline (frame 0 = reply start)."""
        x = C.shift(recs[tid]["x"], lags[tid])
        return {b: C.fe(x, b, n) for b in C.BANDS}

    wearer_lvl = [C.db(np.mean([e for e in C.fe(S.load_trial(t["id"])["x"])[10:]
                                if e > fl * 10]))
                  for t in S.trials(step="2b", done=True) if t.get("level") == "normal"]
    wearer_lvl = float(np.mean(wearer_lvl)) if wearer_lvl else None
    for set_ in ("cur", "rec"):
        rows = {}
        for cid in sorted({t["clip"] for t in ts}):
            n = int(clips[cid]["seconds"] * 50)
            offs = [t["id"] for t in S.trials(step="2a", kind="echo", done=True)
                    if t["clip"] == cid and t["id"] not in weak]
            on = [t["id"] for t in ts if t["kind"] == "echo" and t["clip"] == cid and t["tune"] == set_]
            dts = [t for t in ts if t["kind"] == "dt" and t["clip"] == cid and t["tune"] == set_]
            if not offs or not on:
                continue
            E_off = {b: np.mean([tl(i, n)[b] for i in offs], axis=0) for b in C.BANDS}
            E_on = {b: np.mean([tl(i, n)[b] for i in on], axis=0) for b in C.BANDS}
            act = E_off["full"] > fl * 10
            act[:3] = False
            r = {}
            for b in C.BANDS:
                r["rem_" + b] = float(C.db(E_off[b][act].mean()) - C.db(E_on[b][act].mean()))
            first = act.copy()
            first[50:] = False
            r["rem_first1s"] = float(C.db(E_off["full"][first].mean()) - C.db(E_on["full"][first].mean())) \
                if first.any() else float("nan")
            r["resid_over_floor"] = float(C.db(E_on["full"][act].mean()) - fl_db)
            r["over10"] = float(np.mean(E_on["full"][3:] > fl * 10))
            r["over20"] = float(np.mean(E_on["full"][3:] > fl * 100))
            thr = max(fl * 10, float(np.percentile(E_on["full"][3:], 95)) * 2)
            r["vad_thr_dbfs"] = float(C.db(thr))
            gr = [_col(recs[i], "sup_gate_rel") for i in on]
            gr = [g[g >= 0] for g in gr if g is not None]
            r["gate_rel"] = float(np.mean(np.concatenate(gr))) if gr and sum(map(len, gr)) else float("nan")
            # wearer over echo
            ttp, passf, kept, kept2 = [], [], [], []
            for t in dts:
                rec = recs[t["id"]]
                D = tl(t["id"], n)["full"]
                pm_dt, pm_e = _col(rec, "p_mic"), _col(recs[on[0]], "p_mic")
                cap_dt = _col(rec, "capn")
                cap_e = _col(recs[on[0]], "capn")
                use_pm = (pm_dt is not None and pm_e is not None and np.nanmax(pm_dt) > 0
                          and np.nanmax(pm_e) > 0)
                if use_pm:
                    raw_lag_dt = (lags[t["id"]] - C.AEC_DELAY) / C.SR
                    raw_lag_e = (lags[on[0]] - C.AEC_DELAY) / C.SR
                    td = cap_dt / C.LC3_BPS - raw_lag_dt
                    te = cap_e / C.LC3_BPS - raw_lag_e
                    pe = np.interp(td, te, pm_e)
                for p in t["prompts"]:
                    onset, method = p["at"] + 0.35, "nominal"
                    if use_pm:
                        k = np.nonzero((td >= p["at"] - 0.2) & (td <= p["at"] + 2.5)
                                       & (pm_dt > 2.5 * pe) & (pm_dt > 0))[0]
                        if len(k):
                            onset, method = float(td[k[0]]) - 0.03, "p_mic"
                    k0 = max(0, int(onset * 50) - 3)
                    k1 = min(n, int(onset * 50) + 40)
                    over = np.nonzero(D[k0:k1] > thr)[0]
                    lat = max(0.0, (k0 + over[0]) * 0.02 - onset) * 1000 if len(over) else float("nan")
                    w0, w1 = max(0, int(onset * 50)), min(n, int(onset * 50) + 40)
                    pf = float(np.mean(D[w0:w1] > thr)) if w1 > w0 else float("nan")
                    v_out = np.maximum(D[w0:w1] - E_on["full"][w0:w1], 1e-15).mean() if w1 > w0 else np.nan
                    kr = float("nan")
                    if use_pm:
                        sel = (td >= onset) & (td <= onset + 0.8)
                        raw = np.maximum(pm_dt[sel] - pe[sel], 1e-15).mean() if sel.any() else np.nan
                        kr = float(C.db(v_out) - C.db(raw))
                    k2 = float(C.db(v_out) - wearer_lvl) if wearer_lvl is not None else float("nan")
                    ttp.append(lat)
                    passf.append(pf)
                    kept.append(kr)
                    kept2.append(k2)
                    out["utterances"].append(dict(set=set_, trial=t["id"], cue=p["at"], phrase=p["text"],
                                                  onset=round(onset, 3), onset_method=method,
                                                  ttp_ms=lat, pass_frac=pf, kept_raw=kr, kept_vs_2b=k2))
            r["ttp_ms"] = float(np.nanmedian(ttp)) if np.isfinite(ttp).any() else float("nan")
            r["ttp_missed"] = int(np.sum(~np.isfinite(ttp)))
            r["pass_frac"] = float(np.nanmean(passf)) if passf else float("nan")
            r["kept_raw"] = float(np.nanmean(kept)) if np.isfinite(kept).any() else float("nan")
            r["kept_vs_2b"] = float(np.nanmean(kept2)) if np.isfinite(kept2).any() else float("nan")
            r["n_utt"] = len(ttp)
            rows[cid] = r
        if rows:
            keys = next(iter(rows.values())).keys()
            out["sets"][set_] = {k: float(np.nanmean([rows[c][k] for c in rows])) for k in keys}
            out["sets"][set_]["per_clip"] = rows
    json.dump(out, open(os.path.join(S.root, "results", "step4.json"), "w"), indent=1, default=float)
    return out

# ------------------------------------------------------- step 4 verdict

# Step 4 has the last word on the near end. Its wearer metrics are coarser
# than step 3's (kept is biased by the full-band vs voice-band split; onsets
# come from p_mic every ~60 ms), so its margins are wider: 'rec' fails if it
# keeps the wearer more than 3 dB worse than 'cur' (vs the raw mic, or vs the
# step 2b level), passes them more than 60 ms later (median), or misses more
# phrases.
STEP4_KEPT_MARGIN_DB = 3.0
STEP4_TTP_MARGIN_MS = 60.0


def step4_verdict(sc, sr):
    """'cur' vs 'rec' step 4 scores -> (passed, reasons). passed is None
    when no wearer metric exists on both sides (nothing to judge)."""
    def fin(*v):
        return all(x is not None and np.isfinite(x) for x in v)
    why, n = [], 0
    for k, lbl in (("kept_raw", "wearer kept vs raw mic"), ("kept_vs_2b", "wearer level vs step 2b")):
        a, b = sc.get(k), sr.get(k)
        if fin(a, b):
            n += 1
            if b < a - STEP4_KEPT_MARGIN_DB:
                why.append(f"{lbl} {b:.1f} vs {a:.1f} dB (worse by {a - b:.1f}, margin "
                           f"{STEP4_KEPT_MARGIN_DB:g})")
    a, b = sc.get("ttp_ms"), sr.get("ttp_ms")
    if fin(a, b):
        n += 1
        if b > a + STEP4_TTP_MARGIN_MS:
            why.append(f"median time to pass {b:.0f} vs {a:.0f} ms (later by {b - a:.0f}, margin "
                       f"{STEP4_TTP_MARGIN_MS:g})")
    a, b = sc.get("ttp_missed"), sr.get("ttp_missed")
    if fin(a, b):
        n += 1
        if b > a:
            why.append(f"phrases never passing {b:.0f} vs {a:.0f}")
    if n == 0:
        return None, ["no wearer metric on both sides"]
    return not why, why


def headline(S, s3, s4):
    """The report's verdict: step 4's on-device A/B overrides step 3. Returns
    dict(verdict = 'keep' | 'adopt' | 'unverified' | 'none', rec, tested,
    passed, why)."""
    if not s3:
        return dict(verdict="none")
    rec = s3["recommended"]
    dflt = S.defaults()
    tk = (S.s.get("step4_tune") or {}).get("rec")
    sets = (s4 or {}).get("sets") or {}
    have = "cur" in sets and "rec" in sets and tk is not None
    passed, why = (step4_verdict(sets["cur"], sets["rec"]) if have else (None, []))
    # did step 4 test this recommendation? (keys the firmware lacks are not sent)
    tested_rec = bool(have and dflt and not rec.get("keep_current")
                      and not C.diff(C.apply(dflt, tk), C.apply(dflt, rec.get("tune") or {})))
    out = dict(rec=rec, tested=tk if have else None, passed=passed, why=why, tested_rec=tested_rec)
    if rec.get("keep_current"):
        out["verdict"] = "keep"
    elif tested_rec and passed:
        out["verdict"] = "adopt"
    elif tested_rec:
        out["verdict"] = "rejected"
    else:
        out["verdict"] = "unverified"
    return out

# -------------------------------------------------------------- extras


def run_silero(S):
    files = [S.trial_path(t["id"], "wav") for t in S.trials(done=True)
             if t["kind"] in ("echo", "dt")]
    try:
        r = subprocess.run(["uv", "run", "--script", os.path.join(C.HERE, "silero_vad.py")] + files,
                           capture_output=True, text=True, timeout=900)
        return json.loads(r.stdout.strip().splitlines()[-1])
    except Exception as e:
        return {"error": repr(e)}

# --------------------------------------------------------------- report


def write_report(root, silero=False):
    silero_on = silero
    S = C.Session(root)
    st, dv = S.s["settings"], S.s.get("device", {})
    L = []
    P = L.append
    P(f"# Halo AEC calibration: {st['name']} ({S.s.get('mode', '?')})\n")
    P(f"Session `{os.path.basename(root)}`, started {S.s.get('created', '?')}. Firmware "
      f"`{dv.get('fw', '?')}`. Mic gain {st['mic_gain']} (saved `gain()` reads {dv.get('gain_saved', '?')}, "
      f"effective {dv.get('gain_effective', '?')}"
      + (", restored at the end" if dv.get("gain_restored") else "") + f"), voice=true, AEC off "
      f"for step 2. Speaker LC3 volume {st['volume']}, gain {st['spk_gain']}, budget {st['budget']}."
      + (f" Desk mode: Mac voice at afplay -v {st.get('afplay_volume')}." if st.get("afplay_volume") else ""))
    tab = dv.get("aec_tune")
    P(f"\n`frame.microphone.aec_tune`: " + (f"present, {len(tab)} keys." if tab else
                                            "absent (step 4 skipped)."))
    if S.s.get("floor_dbfs") is not None:
        P(f"Noise floor (step 1, 300–3400 Hz): **{S.s['floor_dbfs']:.1f} dBFS**.")
    plan = S.s.get("plan", [])
    nd = sum(t.get("status") == "done" for t in plan)
    P(f"Recordings: {nd}/{len(plan)} done." + ("" if nd == len(plan) else
                                              " **Incomplete**: `halo_calib.py --resume` continues."))
    # ---- headline: step 4 overrides step 3
    s3p = os.path.join(root, "results", "step3.json")
    s3 = json.load(open(s3p)) if os.path.exists(s3p) else None
    s4 = step4(S)
    hl = headline(S, s3, s4)
    if s4 and s4.get("sets"):
        s4["verdict"] = {k: hl.get(k) for k in ("verdict", "passed", "why", "tested_rec")}
        json.dump(s4, open(os.path.join(root, "results", "step4.json"), "w"), indent=1, default=float)
    P("\n## Recommendation\n")
    sets4 = (s4 or {}).get("sets") or {}
    v = hl["verdict"]

    def s4_numbers():
        sc, sr = sets4.get("cur", {}), sets4.get("rec", {})
        return ("step 4, cur vs rec: echo removed " + _f(sc.get("rem_full")) + " vs " + _f(sr.get("rem_full"))
                + " dB; wearer kept vs raw mic " + _f(sc.get("kept_raw")) + " vs " + _f(sr.get("kept_raw"))
                + " dB; level vs step 2b " + _f(sc.get("kept_vs_2b")) + " vs " + _f(sr.get("kept_vs_2b"))
                + " dB; median time to pass " + _f(sc.get("ttp_ms"), "{:.0f}") + " vs "
                + _f(sr.get("ttp_ms"), "{:.0f}") + " ms; phrases never passing "
                + _f(sc.get("ttp_missed"), "{:.0f}") + " vs " + _f(sr.get("ttp_missed"), "{:.0f}"))

    def keys_of(t):
        return "{" + ", ".join(f"{k}={C.fmt_val(x)}" for k, x in sorted((t or {}).items())) + "}"
    if v == "none":
        P("No recommendation: step 3 has not run.")
    elif v == "keep":
        P("**Keep the current defaults** (`frame.microphone.aec_tune('defaults')`). Step 3 found "
          "nothing that beats them by more than 0.3 dB of objective without losing near end or echo "
          "to them.")
        if hl.get("tested"):
            P(f"\nStep 4 checked the best alternative `{keys_of(hl['tested'])}` on the device: "
              + ("it lost there too (" + "; ".join(hl["why"]) + ")" if hl["passed"] is False else
                 "it did not lose on the near end there; the offline verdict stands"
                 if hl["passed"] else "no wearer metric to judge it")
              + ". " + s4_numbers() + ".")
    elif v == "adopt":
        rec = hl["rec"]
        P("**Adopt the step 3 set**: it passed step 4 on the device (" + s4_numbers() + "). Ready to "
          "paste as one REPL line (back to the defaults, then the keys that differ from them; not "
          "persisted, so it applies until the next reboot):\n")
        P("```lua\n" + rec["lua"] + "\n```")
        P("\nAs compile-time defaults (`modules/halo/src/audio_aec.c`):\n")
        P("```c\n" + (rec["defines"] or "/* no change */") + "\n```")
    elif v == "rejected":
        rec = hl["rec"]
        P("**Keep the current defaults** (`frame.microphone.aec_tune('defaults')`). Step 3's pick "
          "was **rejected by step 4** on the device: " + "; ".join(hl["why"]) + ".\n")
        P(f"Rejected set (step 3, {rec['source']}): `{keys_of(rec.get('tune'))}`; offline echo removed "
          f"{_f(rec['S'].get('echo'))} dB, talker kept {_f(rec['S'].get('kept'), '{:.2f}')} dB. "
          + s4_numbers() + ".")
    else:
        rec = hl["rec"]
        P("**Keep the current defaults for now** (`frame.microphone.aec_tune('defaults')`): step 3's "
          "pick has not passed step 4 on the device"
          + (" (step 4 tested another set)" if hl.get("tested") else " (step 4 not run)")
          + (". This firmware has no `aec_tune`, so step 4 cannot run: verify a build with these "
             "defaults before adopting them" if not tab else
             f". `uv run halo_calib.py --resume {root} --steps 4` tests it") + ".\n")
        P(f"Unverified set (step 3, {rec['source']}): `{keys_of(rec.get('tune'))}`; offline echo removed "
          f"{_f(rec['S'].get('echo'))} dB, talker kept {_f(rec['S'].get('kept'), '{:.2f}')} dB.")
        if hl.get("tested") and hl.get("passed") is False:
            P(f"\nThe set step 4 did test, `{keys_of(hl['tested'])}` (an earlier step 3 pick), was "
              "rejected there: " + "; ".join(hl["why"]) + ". " + s4_numbers() + ".")
        if rec.get("defines"):
            P("\nAs compile-time defaults (unverified on the device):\n")
            P("```c\n" + rec["defines"] + "\n```")
    # ---- captures
    man_p = os.path.join(root, "replay", "manifest.json")
    if os.path.exists(man_p):
        man = json.load(open(man_p))
        P("\n## Step 2: AEC-off captures\n")
        P("| recording | kind | clip | level dBFS | over floor dB | alignment q (window) |")
        P("|---|---|---|---|---|---|")
        fl = S.s.get("floor_dbfs")
        for k, v in man["items"].items():
            lvl = v.get("echo_dbfs", v.get("level_dbfs"))
            P(f"| {k} | {v['kind']}{' (' + v['level'] + ')' if v.get('level') else ''} | "
              f"{v.get('clip') or ''} | {_f(lvl)} | {_f(lvl - fl if (lvl is not None and fl is not None) else None)} | "
              f"{_f(v.get('q_win', v.get('q')), '{:.0f}')}"
              f"{' **weak: not scored**' if v.get('weak_align') else ''} |")
        if man.get("weak_align"):
            P(f"\n**Warning:** the reply did not align in {', '.join(man['weak_align'])} (peak under "
              "15× the median of the search window, no better than chance). Those recordings are "
              "left out of steps 3 and 4: redo them (`--resume ... --redo 2a` / `2c`).")
        cl = S.s.get("clips", {})
        if cl:
            P("\nReply clips (pauses ≥150 ms, s): " + "; ".join(
                f"{c} {v.get('seconds', 0):.1f} s ({v.get('voice', v.get('src', ''))}): "
                + ", ".join(f"{a:.2f}–{b:.2f}" for a, b in v.get("pauses", [])) for c, v in cl.items()))
    # ---- step 3
    if s3:
        P("\n## Step 3: offline replay on these captures\n")
        if s3.get("current_note"):
            P(f"**Note:** {s3['current_note']}.\n")
        P("This tree's `audio_aec.c` replayed on the AEC-off captures (`calib_offline.py`). "
          "Echo = mean over the echo-only recordings; talker = the wearer's own step 2b "
          "captures mixed into the echo captures (exact shadow decomposition); real DT = "
          "step 2c, energy method. Time to pass (ttfp) = step 4's method on the synthetic talker: "
          "onset to the first output frame over that set's VAD threshold (max(floor + 10 dB, p95 "
          "of its echo-only residual + 3 dB)), median over all onsets, a miss (none in 0.8 s) "
          "counted as 800; missed = fraction of onsets missed. Full pass = onset to the first "
          "frame with the talker kept within 6 dB (the gate release time: a closed gate holds "
          "the talker at about −6..−8 dB), median, a miss (none in 1 s) counted as 1000.\n")
        P("| set | echo removed | 300-800 | 800-1600 | 1600-3400 | resid > floor | >floor+10 | "
          ">floor+20 | gate rel. | talker kept | kept 300-800 | crushed | ttfp ms | missed | "
          "full pass ms | real DT kept | aec_tune can set it |")
        P("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
        rows = [(k + (" (= current)" if v.get("same_as_current") and k != "current" else ""),
                 v["S"], v.get("applicable")) for k, v in s3["sets"].items()]
        rows.append(("**step 3 pick**", s3["recommended"]["S"], s3["recommended"]["applicable"]))
        for r in s3.get("rejected_vs_current") or []:
            rows.append((f"{r['source']} (rejected: loses to current)", r["S"],
                         None))
        for name, x, app in rows:
            P(f"| {name} | {_f(x['rem_full'])} | {_f(x['rem_lo'])} | {_f(x['rem_mid'])} | {_f(x['rem_hi'])} | "
              f"{_f(x['resid_over_floor'])} | {_f(x['over10'], '{:.2f}')} | {_f(x['over20'], '{:.2f}')} | "
              f"{_f(x['gate_rel'], '{:.2f}')} | {_f(x['syn_kept_full'], '{:.2f}')} | "
              f"{_f(x['syn_kept_lo'], '{:.2f}')} | {_f(x['syn_crushed'], '{:.3f}')} | "
              f"{_f(x['syn_ttfp_ms'], '{:.0f}')} | {_f(x.get('syn_ttp_missed'), '{:.2f}')} | "
              f"{_f(x.get('syn_tfull_ms'), '{:.0f}')} | {_f(x['rdt_kept_full'], '{:.2f}')} | "
              f"{'–' if app is None else 'yes' if app else 'no'} |")
        P("\n`current` = the device's `aec_tune('defaults')`; `0.8.17` = the 0.8.17 release "
          "source (before the onset, gate and gain fixes); `old-gate` = the full-band gate "
          "defaults that preceded the gate band; `B2-15` = the band-limited gate and two-band "
          "cap (the compiled defaults since); `B15` = B2-15 without the two-band "
          "cap; `A15` = full-band gate, kappa 0.47. Talker kept by level (normal / "
          "quiet / loud) for the recommended set: "
          + " / ".join(_f(s3["recommended"]["S"].get(f"syn_kept_{lv}"), "{:.2f}")
                       for lv in ("normal", "quiet", "loud")) + " dB.")
        P(f"\nRecommendation source: **{s3['recommended']['source']}**"
          + (" (no change from the current defaults)" if s3['recommended'].get('keep_current') else "")
          + f". Objective = echo removed minus penalties for near-end loss beyond the current "
          f"defaults (crushed +0.02, kept −0.5 dB, ttfp +20 ms free); current {_f(s3.get('current_J'), '{:.2f}')}.")
        gl = s3.get("guards") or []
        roles = s3.get("guard_roles") or {}
        P("\nA set other than the defaults is never picked if it loses near end to them (crushed "
          "+0.03, kept −0.3 dB, ttfp +15 ms) or removes more than 1 dB less echo than them, on "
          "this sitting or on any guard sitting; past the near-end "
          "bounds the objective drops 25 dB per bound, so echo gains do not buy a worse talker.")
        for r in s3.get("rejected_vs_current") or []:
            P(f"\nRejected against the current defaults: {r['source']} `"
              + "{" + ", ".join(f"{k}={C.fmt_val(x)}" for k, x in sorted(C.diff(r["params"], s3["defaults"]).items()))
              + "}`: " + "; ".join(r["why"]) + ".")
        P(f"\nGuard sittings (a searched set must not lose on them; only the metrics a sitting "
          "has are checked, and with no near-end check at all the 1.0 dB margin applies): "
          + (", ".join(f"`{os.path.basename(g)}`" + (f" ({roles[g]})" if g in roles else "")
                       for g in gl) if gl else "none, so a searched set "
             "needs a 1.0 dB objective margin instead of 0.5") + "."
          + (" There is no other sitting of this device, so other units' sittings guard "
             "(secondary)." if gl and roles and all(roles.get(g) == "other unit" for g in gl) else ""))
        for fam, r in s3.get("families", {}).items():
            P(f"\nSearch from {fam}: {r.get('searched', 0)} candidates, best objective "
              f"{_f(r.get('best_J'), '{:.2f}')} vs start {_f(r.get('base_J'), '{:.2f}')}; "
              + ("searched set adopted." if r.get("adopted") else "starting set kept.")
              + ("" if not r.get("guard") else " " + "; ".join(r["guard"]) + "."))
        rec = s3["recommended"]
        P("\n### Step 3 pick\n")
        if rec.get("keep_current"):
            P("Keep the current defaults (nothing beat them by more than 0.3 dB of objective "
              "without losing near end or echo to them).")
        else:
            P("`{" + ", ".join(f"{k}={C.fmt_val(x)}" for k, x in sorted((rec.get("tune") or {}).items()))
              + "}` (keys that differ from the defaults). Step 4 decides: see **Recommendation** "
              "above; the paste-ready line is given only for a set that passed it.")
            if not rec.get("applicable"):
                P("\nThis firmware cannot apply all of it (no aec_tune, or keys missing).")
    # ---- step 4
    P("\n## Step 4: AEC on, device defaults vs recommended\n")
    if s4 and s4.get("excluded"):
        P("Left out of step 4 (recorded with a set other than the one below; `--redo 4` "
          "records them again): " + "; ".join(f"{i}: {w}" for i, w in s4["excluded"]) + ".\n")
    if s4 and not s4["sets"]:
        P("No step 4 recording made with the current sets.")
    elif not s4:
        P("Not run" + (" (firmware without `frame.microphone.aec_tune`)." if not tab else "."))
    else:
        tk = S.s.get("step4_tune", {})
        s3c = (s3 or {}).get("step4_candidate") or {}
        dflt = S.defaults() or {}
        now = C.diff(C.apply(dflt, s3c["params"]), dflt) if s3c.get("params") and dflt else None
        stale = now is not None and bool(C.diff(C.apply(dflt, tk.get("rec") or {}), C.apply(dflt, now)))
        P(f"`cur` = `aec_tune('defaults')`; `rec` = `aec_tune{{{', '.join(f'{k}={C.fmt_val(v)}' for k, v in sorted(tk.get('rec', {}).items()))}}}`. "
          f"ABBA order. Floor {s4['floor_dbfs']:.1f} dBFS."
          + (f" Keys this firmware lacks, left out: {', '.join(S.s['step4_dropped_keys'])}."
             if S.s.get("step4_dropped_keys") else "")
          + (" Step 3 recommended keeping the defaults, so `rec` is the best applicable "
             "alternative (a check of that verdict on the device)." if s3c.get("alternative") and not stale
             else "")
          + (" `rec` is an earlier step 3 pick (step 3 was rerun since step 4 was recorded; "
             "`--resume ... --steps 4` tests the current one)." if stale else "")
          + ("" if hl.get("passed") is None else
             f" **Verdict: `rec` {'passes' if hl['passed'] else 'fails'}** the near-end check against `cur` "
             f"(kept −{STEP4_KEPT_MARGIN_DB:g} dB, time to pass +{STEP4_TTP_MARGIN_MS:g} ms, no extra missed "
             "phrases)" + ("" if hl["passed"] else ": " + "; ".join(hl["why"])) + ".") + "\n")
        rowsd = [("echo removed 300-3400 dB", "rem_full", "{:.1f}"), ("  300-800", "rem_lo", "{:.1f}"),
                 ("  800-1600", "rem_mid", "{:.1f}"), ("  1600-3400", "rem_hi", "{:.1f}"),
                 ("  first 1 s of the reply", "rem_first1s", "{:.1f}"),
                 ("residual over floor dB", "resid_over_floor", "{:.1f}"),
                 ("frames > floor+10 dB", "over10", "{:.2f}"), ("frames > floor+20 dB", "over20", "{:.2f}"),
                 ("gate released (echo only)", "gate_rel", "{:.2f}"),
                 ("VAD threshold dBFS", "vad_thr_dbfs", "{:.1f}"),
                 ("wearer: time to pass ms (median)", "ttp_ms", "{:.0f}"),
                 ("wearer: phrases never passing", "ttp_missed", "{:.0f}"),
                 ("wearer: pass fraction, first 0.8 s", "pass_frac", "{:.2f}"),
                 ("wearer: kept vs raw mic dB", "kept_raw", "{:.1f}"),
                 ("wearer: level vs step 2b dB", "kept_vs_2b", "{:.1f}")]
        sc, sr = s4["sets"].get("cur", {}), s4["sets"].get("rec", {})
        P("| metric | cur | rec | rec − cur |")
        P("|---|---|---|---|")
        for lbl, k, fmt in rowsd:
            a, b = sc.get(k), sr.get(k)
            d = (b - a) if (a is not None and b is not None) else None
            P(f"| {lbl} | {_f(a, fmt)} | {_f(b, fmt)} | {_f(d, '{:+.2f}' if 'frac' in k or 'over' in k or 'gate' in k else '{:+.1f}')} |")
        P("\nPer phrase (onset from p_mic unless marked nominal = cue + 0.35 s):\n")
        P("| set | recording | cue s | phrase | onset s | time to pass ms | pass frac | kept vs raw |")
        P("|---|---|---|---|---|---|---|---|")
        for u in s4["utterances"]:
            P(f"| {u['set']} | {u['trial']} | {u['cue']:.1f} | {u['phrase']} | {u['onset']:.2f}"
              f"{'' if u['onset_method'] == 'p_mic' else ' (nominal)'} | {_f(u['ttp_ms'], '{:.0f}')} | "
              f"{_f(u['pass_frac'], '{:.2f}')} | {_f(u['kept_raw'])} |")
    if silero_on:
        P("\n## Silero VAD (v5, 0.6 threshold; fraction of seconds that fire)\n")
        sv = run_silero(S)
        if "error" in sv:
            P(f"Not available: {sv['error']}")
        else:
            P("| recording | fire | mean prob |")
            P("|---|---|---|")
            for k, v in sv.items():
                P(f"| {os.path.basename(k)} | {v['fire']:.2f} | {v['mean']:.3f} |")
    P("\n## Notes\n")
    P("- Step 3 scores one sitting's captures (one fit, one coupling). The search only "
      "replaces its starting set when it wins by > 0.5 dB of objective and does not lose on the "
      "guard sittings, and no set is picked that loses near end or echo to the current defaults on this "
      "or any guard sitting, to limit overfitting. Step 4 on the device has the last word: the "
      "paste-ready line is given only for a set that passed it.")
    if "desk" in S.s.get("mode", ""):
        lv = (S.s.get("settings") or {}).get("desk_voice_dbfs")
        P("- Desk mode: a Mac voice stands in for the wearer"
          + (f" (phrases levelled to {lv:g} dBFS speech, the desk_aec talker clip's level: at "
             "afplay 1.0 about 3 dB under that harness's realistic voice at the mic)" if lv is not None else
             " (raw `say` phrases, 6–9 dB under a realistic voice even at afplay 1.0: near-end "
             "numbers sit at the talker detection floor)")
          + ". It has no bone conduction and one fixed position: use a desk sitting to check the "
          "harness, not to pick a set.")
    P("- Step 4 wearer onsets come from `p_mic` sampled every ~60 ms, so time-to-pass is good to "
      "about ±60 ms; compare `cur` and `rec`, not absolute values.")
    ge = dv.get("gain_effective")
    if dv.get("gain_probe") is not None:
        P(f"- Saved-gain check: reply A at `start{{gain=0}}` read {dv['gain_probe']:+.1f} dB against "
          f"`start{{gain={st['mic_gain']}}}`, so the effective gain was {ge}.")
    if isinstance(ge, str):
        P(f"- `gain()` read 0: the effective mic gain was {st['mic_gain']} if no gain was ever saved on "
          "this unit, or 0 (6 dB lower) if a 0 was saved. Firmware cannot tell these apart.")
    elif ge is not None and ge != st["mic_gain"]:
        P(f"- Effective mic gain was {ge}, not {st['mic_gain']} (a saved gain() overrides start{{gain=}}): "
          "echo coupling and floor are scaled accordingly.")
    P(f"\nFiles: `{root}` (session.json, trials/*.wav|lc3|json, replay/, results/step3.json, "
      "results/step3_candidates.jsonl, results/step4.json, log.txt).")
    rp = os.path.join(root, "report.md")
    open(rp, "w").write("\n".join(L) + "\n")
    return rp


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return
    print(write_report(a[0], silero="--silero" in a))


if __name__ == "__main__":
    main()
