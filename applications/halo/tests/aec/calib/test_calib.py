#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10,<3.13"
# dependencies = ["numpy", "lc3py"]
# ///
"""Device-free tests of the worn-calibration harness (~1-2 minutes).

  uv run test_calib.py [--quick]

1 sets       aec_tune set helpers: paste-ready Lua line relative to the
             device defaults, #defines from the source's macro table,
             readback tolerance; the tree's tune keys cover the named sets
2 replay     the hooked replay build is bit-identical to ../host/aec_wav
             (unhooked) for this tree's audio_aec.c and for 0.8.17; a
             runtime tune changes the output; the shadow of the mic itself
             reproduces the AEC output when nothing is cancelled
3 sim-resume simulated device, 15 % BLE drops, killed after 8 recordings,
             then --resume: all 18 recordings, step 3, step 4, report
4 sim-notune firmware without aec_tune and a saved gain 4 (--gain-policy set):
             step 4 skipped, gain set and restored, report says so; step 3
             again as in a clone without the 0.8.17 tag
5 guard      guard checks count only the metrics that exist; no near-end
             check -> the no-guard margin; same-device sittings guard first
6 align      a recording whose reference does not align is flagged by prep,
             left out of scoring, and the report warns
7 step4-set  a changed step 4 set restarts step 4 on resume; the report
             leaves out recordings of another set (tune_table readback)
8 resume     --resume refuses another --name, a firmware change without
             --redo, and a different gain choice; reuses the recorded one
9 restore    the gain to restore is saved before it is changed; a failed
             aec_tune('defaults') does not skip the gain restore; failures
             are listed with the fix
10 ctrl-c    Ctrl+C at an Enter prompt exits promptly after the cleanup
11 vs-current a searched set that beats its start (and passes the guard vs
             the start) but loses near end to the current defaults on a
             guard sitting is rejected; so is one that loses > 1 dB of echo
             to them there; echo gains never buy a worse talker than the
             defaults beyond the slack
12 step4-wins step 4 overrides step 3: a pick that keeps the wearer > 3 dB
             worse on the device makes the headline "keep the current
             defaults" (shown as rejected, no paste line); a pick that passes
             gets the paste line; desk phrases are at the desk talker level
13 gain-scale the replay applies the session's effective mic gain as the
             firmware does: a capture x2.5 at -g 2.5 gates and cancels like
             it does x1 at gain 1 with the same set (every gate threshold
             follows the gain), and the gain-1 keys alone at x2.5 (no -g)
             open the gate on it; the scale comes from
             gain_effective, else start{gain=}, else a manifest override;
             the keys older firmware ran in mic units are found (0.8.18:
             gate_kappa, gate_absfloor, gate_edge_abs) for step 3 to replay
             'current' / f
14 storage   a full /lfs stops the session before the recording with
             "device storage full: N KB free, need M KB" (free-space probe),
             and a cap.lc3 write that hits ENOSPC stops it too, both with no
             BLE retries and nothing marked done; other write errors retry
15 saved-gain --mic-gain 3 with nothing saved records at gain 3 with no
             saved-gain warning under --gain-policy keep and set, on 0.8.18
             (gain() reads 0: the 2a level probe decides) and on firmware
             with the mic_gain_scale readback (gain() reads its default 0
             or 1: the readback decides, no extra recording); a real saved
             gain equal to that default is still found and handled by the
             policy
"""
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import calib_build as CB  # noqa: E402
import calib_common as C  # noqa: E402
import calib_offline as O  # noqa: E402

FAILS = []


def check(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg, flush=True)
    if not cond:
        FAILS.append(msg)


def t_sets():
    print("1 sets")
    tree = O.tree_defaults()
    check(all(k in tree for k in C.SET_KEYS), f"tree has every set key ({len(tree)} keys)")
    d = C.apply(tree, C.NAMED["B2-15"])
    line = C.lua_tune_line(C.apply(d, C.NAMED["A15"]), d)
    check(line == ("frame.microphone.aec_tune('defaults') "
                   "frame.microphone.aec_tune{cap_split_hz=0, gate_band_hz=0, gate_kappa=0.47}"),
          f"A15 vs B2-15 defaults: {line}")
    line = C.lua_tune_line(C.apply(d, C.NAMED["old-gate"]), d)
    check(line == ("frame.microphone.aec_tune('defaults') "
                   "frame.microphone.aec_tune{cap_split_hz=0, gate_absfloor=0.5, gate_band_hz=0, "
                   "gate_hang_ms=1000, gate_kappa=0.15, sup_beta=1.5, sup_floor=0.1}"), f"old-gate: {line}")
    check(C.lua_tune_line(d, d) == "frame.microphone.aec_tune('defaults')" and "\n" not in line,
          "paste line: defaults first, one line")
    defs = C.c_defines(C.apply(d, dict(gate_hang_ms=1500, gate_kappa=0.3, gate_band_hz=1250)), d)
    check(defs == "#define AEC_SUP_GATE_BAND_HZ 1250\n#define AEC_SUP_GATE_HANG 75\n"
                  "#define AEC_SUP_GATE_KAPPA 0.3f", f"#defines: {defs!r}")
    check(C.apply(d, {"gate_hang_ms": 1210})["gate_hang_ms"] == 1200, "ms keys truncate to 20 ms")
    check(not C.tune_mismatch({"gate_hang_ms": 1210, "gate_kappa": 0.47},
                              {"gate_hang_ms": 1200.0, "gate_kappa": 0.4699999988079}),
          "readback: ms round down to 20, float32 tolerance")
    check(C.tune_mismatch({"gate_kappa": 0.47}, {"gate_kappa": 0.15}) == {"gate_kappa": (0.47, 0.15)},
          "readback: a wrong value is flagged")
    check(C.missing({"gate_band_hz": 1000, "gate_kappa": 0.5}, ["gate_kappa"]) == ["gate_band_hz"],
          "keys a firmware lacks are found")
    line = C.lua_tune_line(C.apply(d, C.NAMED["B3-15"]), d)
    check(line == ("frame.microphone.aec_tune('defaults') "
                   "frame.microphone.aec_tune{cap_hi_gcap=0.15, cap_hi_split_hz=1600}"), f"B3-15 vs B2-15: {line}")
    defs = C.c_defines(C.apply(d, C.NAMED["B3-10"]), d)
    check(defs == "#define AEC_SUP_CAP_HI_SPLIT_HZ 1600", f"top band #define: {defs!r}")
    old = {k: v for k, v in C.apply(tree, C.NAMED["B2-15"]).items()
           if k not in ("cap_hi_split_hz", "cap_hi_gcap")}
    ft = C.firmware_table(C.apply(tree, C.NAMED["B3-10"]), old)
    check(ft["cap_hi_split_hz"] == 0 and not C.diff(ft, d),
          "a firmware without the top band replays with it off")


def _aec_wav(src_text, hdr_dir, tmp, tag, extra=()):
    """../host/aec_wav.c against an unhooked source."""
    s = os.path.join(tmp, f"aec_{tag}.c")
    open(s, "w").write(src_text)
    exe = os.path.join(tmp, f"aec_wav_{tag}")
    subprocess.run([CB._cc(), *CB.CFLAGS, *extra, "-I", CB.HOST, "-I", hdr_dir, "-o", exe, s,
                    os.path.join(CB.HOST, "aec_wav.c"), "-lm"], check=True, capture_output=True)
    return exe


def t_replay(tmp):
    print("2 replay")
    t = CB.build_all()
    # a synthetic capture: clip B through the speaker model, a short IR, noise
    clip = C.read_wav(os.path.join(C.CLIPS_DIR, "replies", "B.wav"))
    ref = O.spk_ref(C.lc3_decode(C.lc3_encode(clip)), 100, 6, 100, tmp, "B")
    rng = np.random.default_rng(1)
    n = len(ref) + 2 * C.SR
    r = np.zeros(n)
    r[C.SR:C.SR + len(ref)] = ref
    ir = rng.standard_normal(64) * np.exp(-np.arange(64) / 12.0)
    ir[0] += 2
    ir *= 0.4 / np.sqrt((ir ** 2).sum())
    mic = C.bandpass(np.convolve(r, ir)[:n] + 3e-4 * rng.standard_normal(n), 300, 3400)
    mp, rp = os.path.join(tmp, "m.wav"), os.path.join(tmp, "r.wav")
    C.write_wav(mp, mic)
    C.write_wav(rp, np.roll(r, -24))

    def out_of(cmd):
        o = os.path.join(tmp, "o.wav")
        subprocess.run(cmd + [mp, rp, o], check=True, capture_output=True)
        return C.read_wav(o)
    cur = open(CB.AEC_SRC).read()
    ref_wav = out_of([_aec_wav(cur, CB.AEC_INC, tmp, "cur")])
    hooked = out_of([t["aec_replay"]])
    check(np.array_equal(ref_wav, hooked), "this tree: hooked replay == unhooked aec_wav (bit-exact)")
    rem = C.db(C.fe(mic).sum()) - C.db(C.fe(hooked).sum())
    check(rem > 6, f"the AEC removes echo on the synthetic capture ({rem:.1f} dB)")
    tuned = out_of([t["aec_replay"], "-t", "sup_floor=0.3,gate_kappa=0.3"])
    check(not np.array_equal(tuned, hooked), "a runtime tune changes the output")
    if t["aec_replay_0817"]:
        src = CB._git_show("modules/halo/src/audio_aec.c").decode()
        hd = os.path.join(tmp, "inc0817", "halo")
        os.makedirs(hd)
        open(os.path.join(hd, "audio_aec.h"), "wb").write(CB._git_show("modules/halo/include/halo/audio_aec.h"))
        a = out_of([_aec_wav(src, os.path.dirname(hd), tmp, "0817")])
        b = out_of([t["aec_replay_0817"]])
        check(np.array_equal(a, b), "0.8.17: hooked replay == unhooked aec_wav (bit-exact)")
        check(not np.array_equal(a, hooked), "0.8.17 and this tree differ")
    else:
        print("  skip 0.8.17 (no tag in this clone)")
    # shadow: a near-end-only signal that IS the mic, with a silent reference,
    # is passed by the suppressor exactly as the mic is
    z = os.path.join(tmp, "z.wav")
    C.write_wav(z, np.zeros(n))
    so, oo = os.path.join(tmp, "so.wav"), os.path.join(tmp, "oo.wav")
    subprocess.run([t["aec_replay"], "-f", f"{C.SR},{n - C.SR}", "-s", mp, "-o", so, mp, z, oo],
                   check=True, capture_output=True)
    a, b = C.read_wav(so), C.read_wav(oo)
    err = np.abs(a - b).max() * 32768
    check(err <= 2, f"shadow of the mic == AEC output with a silent reference (max {err:.0f} LSB)")


def t_gain_scale(tmp):
    print("13 gain-scale")
    want = {1: 1.0, 4: 2.5, 3: 2.0, 0: 0.5, -1: 0.25, -10: 32 / 704, 10: 5.5, 12: 5.5}
    got = {g: C.mic_gain_scale(g) for g in want}
    check(got == want, f"gain step -> scale over gain 1 (dmic_gain_raw / 704): {got}")
    # a hot synthetic capture (H2-like distortion left for the suppressor)
    clip = C.read_wav(os.path.join(C.CLIPS_DIR, "replies", "B.wav"))
    ref = O.spk_ref(C.lc3_decode(C.lc3_encode(clip)), 100, 6, 100, tmp, "Bg")
    rng = np.random.default_rng(1)
    n = len(ref) + 2 * C.SR
    r = np.zeros(n)
    r[C.SR:C.SR + len(ref)] = ref
    ir = rng.standard_normal(64) * np.exp(-np.arange(64) / 12.0)
    ir[0] += 2
    ir *= 0.4 / np.sqrt((ir ** 2).sum())
    e = np.convolve(r, ir)[:n]
    mic = 2.0 * C.bandpass(e + 3.0 * e * np.abs(e) + 3e-4 * rng.standard_normal(n), 300, 3400)
    rp, m1, m25 = (os.path.join(tmp, f) for f in ("rg.wav", "m1.wav", "m25.wav"))
    C.write_wav(rp, np.roll(r, -24))
    C.write_wav(m1, mic / 2.5)
    C.write_wav(m25, mic)
    d = O.tree_defaults()
    fd = [C.SR, n - C.SR]
    a = O.run(m25, rp, d, feed=fd, dump=True, gain_scale=2.5)
    c = O.run(m25, rp, d, feed=fd, dump=True)
    g1 = O.run(m1, rp, d, feed=fd, dump=True)
    ga, gc, g1g = a["dump"]["gate_rel"], c["dump"]["gate_rel"], g1["dump"]["gate_rel"]
    err = 10 * np.log10(((a["out"] / 2.5 - g1["out"]) ** 2).sum() / (g1["out"] ** 2).sum())
    errc = 10 * np.log10(((c["out"] / 2.5 - g1["out"]) ** 2).sum() / (g1["out"] ** 2).sum())
    check(np.abs(mic).max() < 0.99 and np.array_equal(ga, g1g) and err < -30 and
          gc.sum() > g1g.sum() + 20 and errc > err + 10,
          f"x2.5 at -g 2.5 gates as x1 at gain 1 ({int(ga.sum())} vs {int(g1g.sum())} open, output "
          f"{err:.0f} dB); without -g the gain-1 keys open the gate on {int(gc.sum())}/{len(gc)} "
          f"blocks, output {errc:.0f} dB")
    # where the scale comes from
    sd = os.path.join(tmp, "gs_sess")
    rd = os.path.join(sd, "replay")
    os.makedirs(rd)
    man = {"session": sd, "items": {}}
    json.dump(man, open(os.path.join(rd, "manifest.json"), "w"))
    res = []
    for dev, mg in (({"gain_effective": 4}, 1), ({"gain_effective": "3 or 0"}, 3), ({}, 1)):
        json.dump({"settings": {"mic_gain": mg}, "device": dev}, open(os.path.join(sd, "session.json"), "w"))
        res.append(O.session_gain_scale(rd))
    man["mic_gain_scale"] = 2.0
    json.dump(man, open(os.path.join(rd, "manifest.json"), "w"))
    res.append(O.session_gain_scale(rd))
    check(res == [2.5, 2.0, 1.0, 2.0],
          f"scale from gain_effective 4, else start{{gain=3}}, else 1; manifest override 2.0: {res}")
    # what older firmware ran in mic units ('current' replays them / f)
    tree = O.tree_defaults()
    v0818 = {k: v for k, v in tree.items() if not k.startswith(("gate_kappa_hf", "gate_hf_", "dtd_"))}
    v0818.update(gate_kappa=0.5, sup_beta=1.25)
    mid = dict(tree, gate_kappa=0.5, sup_beta=1.25)
    mu = [C.mic_unit_keys(dv) for dv in ({"aec_defaults": v0818}, {"aec_defaults": v0818, "gain_scale_probe": 1.0},
                                         {"aec_defaults": mid}, {"aec_defaults": tree}, {})]
    t = C.to_gain1(dict(gate_kappa=0.5, gate_absfloor=0.9, sup_beta=1.0), mu[0], 2.5)
    check(mu == [C.MIC_UNIT_KEYS_0818, [], ["gate_kappa"], [], []]
          and C.same(t["gate_kappa"], 0.2) and C.same(t["gate_absfloor"], 0.36) and t["sup_beta"] == 1.0,
          f"mic-unit keys: 0.8.18 kappa/absfloor/edge_abs, first readback build none, unscaled-kappa "
          f"builds kappa, this tree none ({mu}); / 2.5: {t}")


def sim(tmp, *args):
    cmd = [sys.executable, os.path.join(HERE, "halo_calib.py"), "--sim", "--fast", "--sessions",
           os.path.join(tmp, "sessions"), "--search-budget", "10", "--no-guard"] + list(args)
    return subprocess.run(cmd, capture_output=True, text=True)


def latest(tmp):
    d = os.path.join(tmp, "sessions")
    return os.path.join(d, sorted(os.listdir(d))[-1])


def t_sim_resume(tmp):
    print("3 sim-resume")
    r = sim(tmp, "--sim-drop", "0.15", "--sim-abort-after", "8", "--seed", "3")
    check(r.returncode == 2 and "--resume latest" in r.stdout,
          f"abort exits 2 with a resume hint ({r.returncode}) {r.stderr[-300:]}")
    sd = latest(tmp)
    s = json.load(open(os.path.join(sd, "session.json")))
    n1 = sum(t["status"] == "done" for t in s["plan"])
    check(n1 == 7, f"7 recordings kept after the kill at the 8th ({n1})")
    log = open(os.path.join(sd, "log.txt")).read()
    check("attempt 2" in log, "BLE drops were retried within the step")
    r = sim(tmp, "--resume", "latest", "--sim-drop", "0.15", "--seed", "4")
    check(r.returncode == 0, f"resume completes ({r.returncode}) {r.stderr[-300:]}")
    s = json.load(open(os.path.join(sd, "session.json")))
    check(all(t["status"] == "done" for t in s["plan"]) and len(s["plan"]) == 19,
          "all 18 recordings + the saved-gain probe done")
    check(s["device"].get("gain_effective") == 1 and s["device"].get("gain_probe", 0) < -3,
          f"gain() 0 + probe {s['device'].get('gain_probe')} dB: no saved gain, gain 1 in effect")
    check(s.get("step3_done"), "step 3 done")
    s4 = json.load(open(os.path.join(sd, "results", "step4.json")))
    check(set(s4["sets"]) == {"cur", "rec"} and len(s4["utterances"]) == 4, "step 4 A/B with 4 phrases")
    check(all(u["onset_method"] == "p_mic" for u in s4["utterances"]), "wearer onsets found from p_mic")
    rep = open(os.path.join(sd, "report.md")).read()
    check("rec − cur" in rep and ("aec_tune{" in rep or "Keep the current defaults" in rep),
          "report has the A/B table and a Lua line or a keep verdict")
    s3 = json.load(open(os.path.join(sd, "results", "step3.json")))
    check(set(s3["sets"]) >= {"current", "old-gate", "B2-15", "B3-10", "B3-15", "S15", "B15", "A15"},
          f"compared sets {sorted(s3['sets'])}")
    cl = [json.loads(ln) for ln in open(os.path.join(sd, "results", "step3_candidates.jsonl"))]
    check(any(r["tag"].startswith("top band") for r in cl), "the search tries the top cap band")
    for fam in {r["family"] for r in cl}:
        fr = [r for r in cl if r["family"] == fam]
        top = [r for r in fr if r["tag"].startswith("top band") or r["tag"] == "base"]
        best_top = max(top, key=lambda r: r["J"])["params"]
        grid = [r for r in fr if r["tag"].startswith("grid")]
        check(grid and all(r["params"]["cap_hi_split_hz"] == best_top["cap_hi_split_hz"]
                           and C.same(r["params"]["cap_hi_gcap"], best_top["cap_hi_gcap"]) for r in grid),
              f"family {fam}: the kappa x floor grid starts from the best set after the top band "
              f"step ({best_top['cap_hi_split_hz']} / {best_top['cap_hi_gcap']})")
    check(s3["step4_candidate"] and s.get("step4_tune", {}).get("rec"),
          f"step 4 tests a changed set ({s.get('step4_tune', {}).get('rec')})")
    tr = json.load(open(os.path.join(sd, "trials", "2c-dt-A.json")))
    check(tr["diag"] and tr["prompts"] and tr["settings"]["volume"] == 100, "trial json has diag, prompts, settings")
    return sd


def t_sim_notune(tmp):
    print("4 sim-notune")
    r = sim(tmp, "--sim-no-tune", "--sim-saved-gain", "4", "--gain-policy", "set", "--seed", "5")
    check(r.returncode == 0, f"session completes ({r.returncode}) {r.stderr[-300:]}")
    sd = latest(tmp)
    s = json.load(open(os.path.join(sd, "session.json")))
    check(not any(t["status"] == "done" for t in s["plan"] if t["step"] == "4"), "step 4 not recorded")
    check(s["device"].get("gain_effective") == 1 and s["device"].get("gain_restored"),
          "saved gain 4 set to 1 for the session and restored")
    s3 = json.load(open(os.path.join(sd, "results", "step3.json")))
    check(not s3["recommended"]["applicable"], "nothing applicable without aec_tune")
    rep = open(os.path.join(sd, "report.md")).read()
    check("firmware without `frame.microphone.aec_tune`" in rep, "report says step 4 was skipped")
    return sd


def sim_env(tmp, env, *args):
    cmd = [sys.executable, os.path.join(HERE, "halo_calib.py"), "--sim", "--fast", "--sessions",
           os.path.join(tmp, "sessions"), "--search-budget", "10", "--no-guard"] + list(args)
    return subprocess.run(cmd, capture_output=True, text=True, env=dict(os.environ, **env))


def copy_session(tmp, src, name):
    dst = os.path.join(tmp, "copies", name)
    shutil.copytree(src, dst)
    return dst


def t_tagless(tmp, sd):
    """Step 3 on firmware without aec_tune, in a clone without the 0.8.17 tag."""
    r = subprocess.run([sys.executable, os.path.join(HERE, "calib_offline.py"), "step3", sd,
                        "--budget", "5", "--no-guard"], capture_output=True, text=True,
                       env=dict(os.environ, AEC_CALIB_REF_TAG="no-such-tag"))
    check(r.returncode == 0, f"tagless clone: step 3 completes ({r.returncode}) {r.stderr[-300:]}")
    s3 = json.load(open(os.path.join(sd, "results", "step3.json")))
    check(C.REF_SET not in s3["sets"] and isinstance(s3["sets"]["current"]["params"], dict)
          and "no-such-tag" in (s3.get("current_note") or ""),
          f"tagless clone: 'current' falls back to old-gate with a note ({s3.get('current_note')})")
    import calib_report
    rep = open(calib_report.write_report(sd)).read()
    check("**Note:** firmware without aec_tune and no no-such-tag tag" in rep, "report carries the note")


def t_guard(tmp):
    print("5 guard")
    nan = float("nan")
    b = dict(echo=15.0, crushed=nan, kept=nan, ttfp=nan)
    ok, n, n_ne, bad = O.guard_check(dict(echo=15.5, crushed=nan, kept=nan, ttfp=nan), b)
    check(ok and n == 1 and n_ne == 0, f"NaN near end: only echo is checked ({n} checks, {n_ne} near end)")
    ok, n, n_ne, bad = O.guard_check(dict(echo=13.0, crushed=0.1, kept=-1.0, ttfp=nan),
                                     dict(echo=15.0, crushed=0.0, kept=-0.5, ttfp=nan))
    check(not ok and n == 3 and n_ne == 2 and bad == ["echo", "crushed", "kept"], f"losses found {bad}")
    gx = {"g1": dict(echo=15.5, crushed=nan, kept=nan, ttfp=nan)}
    gb = {"g1": b}
    ok, lines, n_all, ne = O.guard_verdict(10.7, 10.0, gx, gb, {"g1": "other unit"})
    check(not ok and ne == 0 and any("no near-end guard check" in ln for ln in lines),
          "a guard with no near-end metric does not pass a 0.7 dB win")
    ok, lines, n_all, ne = O.guard_verdict(11.2, 10.0, gx, gb, {"g1": "other unit"})
    check(ok and n_all == 1 and "1 guard checks ran (0 near end)" in lines[-1],
          f"... but passes a win over the 1.0 dB margin, and says how many checks ran: {lines[-1]}")
    # guard sittings: same device first, other units only when there is none
    root = os.path.join(tmp, "guard_sessions")

    def mk(d, name, mode="desk"):
        os.makedirs(os.path.join(root, d))
        plan = [dict(kind="echo", step="2a", status="done"), dict(kind="wearer", step="2b", status="done")]
        json.dump(dict(settings=dict(name=name), mode=mode, plan=plan),
                  open(os.path.join(root, d, "session.json"), "w"))
        return os.path.join(root, d)
    me = mk("3-me", "Halo A", "worn")
    other = mk("1-other", "Halo B")
    mk("0-sim", "Halo A", "sim desk")
    check(O.guard_sessions(me, roles=True) == [(other, "other unit")],
          "no same-device sitting: other units guard (labelled)")
    same = mk("2-same", "Halo A")
    check(O.guard_sessions(me, roles=True) == [(same, "same device")],
          "a same-device sitting is the guard; other units and sim sittings are not")


def t_align(tmp, src):
    print("6 align")
    sd = copy_session(tmp, src, "align")
    shutil.rmtree(os.path.join(sd, "replay"), ignore_errors=True)
    S = C.Session(sd)
    tid = "2a-echo-B-r1"
    x = S.load_trial(tid)["x"]
    rng = np.random.default_rng(7)
    C.write_wav(S.trial_path(tid, "wav"), 3e-3 * C.bandpass(rng.standard_normal(len(x)), 300, 3400))
    man = O.prep(sd)
    it = man["items"]
    check(it[tid]["weak_align"] and man["weak_align"] == [tid],
          f"no echo in {tid}: flagged weak (q_win {it[tid]['q_win']:.1f}); others "
          f"{min(v['q_win'] for k, v in it.items() if 'q_win' in v and k != tid):.0f}+")
    jobs = O.build_jobs(os.path.join(sd, "replay"))
    check(not any(j["id"] == tid or j["id"].startswith(tid + "+") for j in jobs) and jobs,
          "the weak recording is not scored (echo or synthetic)")
    import calib_report
    rep = open(calib_report.write_report(sd)).read()
    check("weak: not scored" in rep and "**Warning:** the reply did not align in 2a-echo-B-r1" in rep,
          "the report warns")


def t_step4_set(tmp, src):
    print("7 step4-set")
    import calib_report
    sd = copy_session(tmp, src, "step4set")
    S = C.Session(sd)
    keep, excl = calib_report.step4_trials(S)
    check(len(keep) == 4 and not excl, f"all 4 step 4 recordings match their set ({len(keep)}, {excl})")
    rid = next(t["id"] for t in S.trials(step="4", done=True) if t["tune"] == "rec")
    p = S.trial_path(rid, "json")
    rec = json.load(open(p))
    rec["tune_table"]["gate_kappa"] = 0.123
    json.dump(rec, open(p, "w"))
    keep, excl = calib_report.step4_trials(S)
    check(len(keep) == 3 and excl and excl[0][0] == rid and "gate_kappa" in excl[0][1],
          f"a recording of another set is left out: {excl}")
    rep = open(calib_report.write_report(sd)).read()
    check(f"Left out of step 4" in rep and rid in rep, "the report lists it")
    # step 3's pick changed since step 4 was recorded: a resume records step 4 again
    sd = copy_session(tmp, src, "step4redo")
    S = C.Session(sd)
    want = S.s["step4_tune"]
    S.s["step4_tune"] = {"cur": {}, "rec": {"gate_kappa": 0.3}}
    S.save()
    r = sim(tmp, "--resume", sd, "--steps", "4", "--seed", "8")
    check(r.returncode == 0, f"resume completes ({r.returncode}) {r.stderr[-300:]}")
    log = open(os.path.join(sd, "log.txt")).read().split("session start (resume)")[-1]
    S = C.Session(sd)
    redone = [ln for ln in log.splitlines() if ln[9:].startswith("trial 4-") and " done " in ln]
    check("step 4 set changed" in log and len(redone) == 4 and S.s["step4_tune"] == want,
          f"step 4 recorded again with the current pick ({len(redone)} recordings)")


def t_resume_checks(tmp, src, notune):
    print("8 resume")
    sd = copy_session(tmp, src, "resume")
    r = sim(tmp, "--resume", sd, "--name", "Halo 99", "--steps", "1")
    check(r.returncode != 0 and "not 'Halo 99'" in r.stderr, f"another --name is refused {r.stderr[-200:]}")
    r = sim(tmp, "--resume", sd, "--sim-fw", "sim-0.8.19", "--steps", "1")
    s = json.load(open(os.path.join(sd, "session.json")))
    check(r.returncode != 0 and "--redo 1,2a,2b,2c,4" in r.stderr and s["device"]["fw"] == "sim-0.8.18",
          f"a firmware change needs --redo ({r.stderr.strip().splitlines()[-1][:160]})")
    r = sim(tmp, "--resume", sd, "--sim-fw", "sim-0.8.19", "--redo", "1,2a,2b,2c,4", "--steps", "1")
    s = json.load(open(os.path.join(sd, "session.json")))
    check(r.returncode == 0 and s["device"]["fw"] == "sim-0.8.19",
          f"... and passes with it ({r.returncode}) {r.stderr[-200:]}")
    # gain: the no-tune sitting set saved gain 4 to 1 (--gain-policy set)
    sd = copy_session(tmp, notune, "resume-gain")
    r = sim(tmp, "--resume", sd, "--sim-no-tune", "--sim-saved-gain", "4", "--gain-policy", "keep",
            "--steps", "1")
    check(r.returncode != 0 and "--gain-policy set" in r.stderr, f"another gain choice is refused "
          f"{r.stderr.strip()[-200:]}")
    r = sim(tmp, "--resume", sd, "--sim-no-tune", "--sim-saved-gain", "4", "--steps", "1")
    s = json.load(open(os.path.join(sd, "session.json")))
    check(r.returncode == 0 and "this session's gain choice: set" in r.stdout
          and s["device"].get("gain_restored") and s["device"]["gain_effective"] == 1,
          f"the recorded choice is reused ({r.returncode}) {r.stderr[-200:]}")


def t_restore(tmp):
    print("9 restore")
    r = sim(tmp, "--sim-no-tune", "--sim-saved-gain", "4", "--gain-policy", "set", "--sim-fail",
            "set_gain", "--steps", "1")
    sd = latest(tmp)
    s = json.load(open(os.path.join(sd, "session.json")))
    check(r.returncode != 0 and s["device"].get("gain_restore") == 4 and not s["device"].get("gain_restored"),
          "the gain to restore is in session.json before the change is attempted")
    check("Not restored" in r.stdout and "frame.microphone.gain(4)" in r.stdout
          and f"--resume {sd} --steps none" in r.stdout, "the failure is listed with the fix")
    r = sim(tmp, "--sim-saved-gain", "4", "--gain-policy", "set", "--sim-fail", "finish_tune",
            "--steps", "1")
    sd = latest(tmp)
    s = json.load(open(os.path.join(sd, "session.json")))
    check(r.returncode == 0 and s["device"].get("gain_restored") and "gain_restore" not in s["device"],
          f"a failed aec_tune('defaults') does not skip the gain restore ({r.returncode})")
    check("aec_tune defaults" in r.stdout and "frame.microphone.aec_tune('defaults')" in r.stdout
          and "saved mic gain" not in r.stdout.split("Not restored")[-1],
          "only aec_tune is listed, with its fix")


def t_ctrl_c(tmp):
    print("10 ctrl-c")
    code = ("import asyncio, sys\n"
            f"sys.path.insert(0, {HERE!r})\n"
            "import halo_calib as H\n"
            "class A: desk = sim = fast = False\n"
            "async def main():\n"
            "    ui = H.UI(A())\n"
            "    try:\n"
            "        await ui.enter()\n"
            "    finally:\n"
            "        print('cleanup done', flush=True)\n"
            "try:\n"
            "    asyncio.run(main())\n"
            "except KeyboardInterrupt:\n"
            "    print('interrupted', flush=True)\n")
    p = subprocess.Popen([sys.executable, "-c", code], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True)
    time.sleep(2.0)
    t0 = time.monotonic()
    p.send_signal(signal.SIGINT)
    try:
        p.wait(timeout=10)      # not communicate(): it closes stdin, which ends input() too
        dt = time.monotonic() - t0
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
        dt = 99
    out, err = p.stdout.read(), p.stderr.read()
    p.stdin.close()
    check(dt < 3 and "cleanup done" in out and "interrupted" in out,
          f"Ctrl+C at a prompt (stdin open) exits after cleanup in {dt:.1f} s {err[-200:]}")


def t_vs_current(tmp):
    print("11 vs-current")
    nan = float("nan")
    anchor = dict(echo=12.0, kept=-6.0, crushed=0.20, ttfp=300.0)
    big_echo = dict(anchor, echo=40.0, kept=-7.0)
    check(not O.feasible(big_echo, anchor) and O.objective(big_echo, anchor) < O.objective(anchor, anchor),
          f"+28 dB echo does not buy a talker 1 dB worse than the defaults (J "
          f"{O.objective(big_echo, anchor):.1f} vs {O.objective(anchor, anchor):.1f})")
    check(O.feasible(dict(anchor, echo=13.0, kept=-6.25, crushed=0.22, ttfp=310), anchor)
          and O.feasible(dict(anchor, kept=nan), anchor), "within the slack (or unscored) is feasible")
    check(O.near_end_loss(dict(anchor, crushed=0.24, ttfp=320), anchor) == ["crushed", "ttfp"],
          "crushed +0.04 and ttfp +20 ms are losses")
    # a fake replay: on the session every set keeps the talker as the
    # defaults do and a lower kappa removes more echo; on the guard sitting
    # every set matches the A15-like start (so the guard vs the start
    # passes) but keeps the talker 2 dB worse than the current defaults
    sess = os.path.join(tmp, "vc_sess", "replay")
    g = os.path.join(tmp, "vc_guard")
    os.makedirs(os.path.join(g, "replay"))
    open(os.path.join(g, "replay", "manifest.json"), "w").write("{}")
    cur = dict(sup_beta=1.25, sup_floor=0.15, steady_gcap=0.25, gate_kappa=0.5, gate_absfloor=0.9,
               gate_edge_abs=1.5, gate_hang_ms=1200, gate_band_hz=1000, cap_split_hz=750,
               cap_lo_gcap=0.5)
    base = dict(cur, gate_kappa=0.47, gate_band_hz=0, cap_split_hz=0)
    if "cap_hi_split_hz" in O.SPACE:
        cur.update(cap_hi_split_hz=1600, cap_hi_gcap=0.1)
        base.update(cap_hi_split_hz=0, cap_hi_gcap=0.1)

    def fake_eval(data, p):
        if p == cur:
            S = dict(anchor) if data == sess else dict(echo=15.0, kept=-5.0, crushed=0.2, ttfp=300.0)
        elif data == sess:
            S = dict(anchor, echo=12.0 + 20 * (0.47 - p["gate_kappa"]) + (2 if p.get("cap_hi_split_hz") else 0))
        else:
            S = dict(echo=15.0, kept=-7.0, crushed=0.2, ttfp=300.0)
        return S, {}
    real = O.evaluate
    O.evaluate = fake_eval
    O._GUARD_CACHE.clear()
    try:
        out = {}
        for tag, c in (("old", None), ("new", cur)):
            rows, logs = [], []
            out[tag] = (O.search(sess, "A15", base, time.time() + 5, logs.append, rows, anchor, [g],
                                 {g: "same device"}, current=c), logs)
    finally:
        O.evaluate = real
        O._GUARD_CACHE.clear()
    r_old, r_new = out["old"][0], out["new"][0]
    check(r_old["adopted"] and r_old["J"] > r_old["base_J"] + 0.5,
          f"the set beats its start by {r_old['J'] - r_old['base_J']:.1f} dB and passes the guard vs "
          "the start (the old rule adopts it)")
    log = "\n".join(out["new"][1])
    check(not r_new["adopted"] and r_new["params"] == base and "vs current: LOSES kept" in log,
          "... but it loses near end to the current defaults on the guard sitting: rejected")
    # the same on echo: on the guard sitting every set keeps the near end
    # of the current defaults and matches the start, but removes 1.5 dB less
    # echo than the defaults (the guard vs the start alone passes it)
    check(O.echo_loss(dict(echo=13.9), dict(echo=15.0)) == ["echo"]
          and O.echo_loss(dict(echo=14.1), dict(echo=15.0)) == []
          and O.echo_loss(dict(echo=nan), dict(echo=15.0)) == [], "echo -1.1 dB is a loss, -0.9 not")
    out = {}
    for tag, g_echo in (("lose", 13.5), ("ok", 14.5)):
        def fake_eval2(data, p, g_echo=g_echo):
            if p == cur:
                S = dict(anchor) if data == sess else dict(echo=15.0, kept=-5.0, crushed=0.2, ttfp=300.0)
            elif data == sess:
                S = dict(anchor, echo=12.0 + 20 * (0.47 - p["gate_kappa"])
                         + (2 if p.get("cap_hi_split_hz") else 0))
            else:
                S = dict(echo=g_echo, kept=-5.0, crushed=0.2, ttfp=300.0)
            return S, {}
        O.evaluate = fake_eval2
        O._GUARD_CACHE.clear()
        try:
            rows, logs = [], []
            out[tag] = (O.search(sess, "A15", base, time.time() + 5, logs.append, rows, anchor, [g],
                                 {g: "same device"}, current=cur), logs)
        finally:
            O.evaluate = real
            O._GUARD_CACHE.clear()
    log = "\n".join(out["lose"][1])
    check(not out["lose"][0]["adopted"] and out["lose"][0]["params"] == base
          and "vs current: LOSES echo" in log and "LOSES kept" not in log,
          "a set that beats its start and keeps the near end but removes 1.5 dB less echo than the "
          "current defaults on a guard sitting is rejected (logged as an echo loss)")
    check(out["ok"][0]["adopted"], "... and adopted when it is within 1 dB of them there")


def t_step4_wins(tmp, src):
    print("12 step4-wins")
    import calib_report as R
    sd = copy_session(tmp, src, "step4wins")
    S = C.Session(sd)
    dflt = S.defaults()
    tk = S.s["step4_tune"]["rec"]
    p3 = os.path.join(sd, "results", "step3.json")
    s3 = json.load(open(p3))
    pick = C.apply(dflt, tk)
    lua = C.lua_tune_line(pick, dflt)
    s3["recommended"].update(source="family A15", params=pick, keep_current=False, lua=lua,
                             tune=C.diff(pick, dflt), defines=C.c_defines(pick, dflt), applicable=True)
    json.dump(s3, open(p3, "w"))
    cur = dict(rem_full=15.8, kept_raw=-7.9, kept_vs_2b=-9.6, ttp_ms=0.0, ttp_missed=0, pass_frac=0.26)
    bad = dict(rem_full=24.1, kept_raw=-17.7, kept_vs_2b=-22.4, ttp_ms=187.0, ttp_missed=0, pass_frac=0.16)
    good = dict(rem_full=19.0, kept_raw=-8.6, kept_vs_2b=-10.1, ttp_ms=40.0, ttp_missed=0, pass_frac=0.25)
    ok, why = R.step4_verdict(cur, bad)
    check(ok is False and len(why) == 3, f"EC-like step 4 fails: {why}")
    check(R.step4_verdict(cur, good)[0] is True, "a pick within the margins passes")
    real = R.step4
    try:
        for name, rec4 in (("bad", bad), ("good", good)):
            R.step4 = lambda S_, r=rec4: {"floor_dbfs": -70.0, "sets": {"cur": dict(cur), "rec": dict(r)},
                                          "utterances": [], "excluded": []}
            rep = open(R.write_report(sd)).read()
            head = rep.split("## Recommendation")[1].split("\n## ")[0]
            if name == "bad":
                check("**Keep the current defaults**" in head and "rejected by step 4" in head
                      and "-17.7" in head and "```lua" not in rep and lua not in rep,
                      "step 4 rejects the pick: headline keeps the defaults, the set shown as rejected "
                      "with its numbers, no paste line")
            else:
                check("**Adopt the step 3 set**" in head and "```lua\n" + lua in head,
                      "a pick that passed step 4 gets the paste line")
    finally:
        R.step4 = real
    # an unverified pick (step 4 tested another set) gets no paste line either
    S = C.Session(sd)
    S.s["step4_tune"]["rec"] = {"gate_kappa": 0.123}
    S.save()
    R.step4 = lambda S_: {"floor_dbfs": -70.0, "sets": {"cur": dict(cur), "rec": dict(good)},
                          "utterances": [], "excluded": []}
    try:
        rep = open(R.write_report(sd)).read()
    finally:
        R.step4 = real
    check("Keep the current defaults for now" in rep and "```lua" not in rep,
          "a pick step 4 never tested: keep the defaults for now, no paste line")
    # desk phrases (if made on this Mac) sit at the desk talker's speech level
    d = os.path.join(C.CLIPS_DIR, "desk_voice")
    if os.path.exists(os.path.join(d, "level.json")):
        lv = [C.active_rms_db(C.read_wav(os.path.join(d, f))) for f in os.listdir(d) if f.endswith(".wav")]
        check(lv and max(abs(v - C.DESK_VOICE_LEVEL_DBFS) for v in lv) < 0.6,
              f"desk phrases at {C.DESK_VOICE_LEVEL_DBFS} dBFS speech level ({min(lv):.1f}..{max(lv):.1f})")
    x = C.level_desk_phrase(0.01 * np.sin(np.arange(C.SR) * 2 * np.pi * 440 / C.SR)
                            * (1 + 4 * (np.arange(C.SR) % 4000 < 200)))
    check(abs(C.active_rms_db(x) - C.DESK_VOICE_LEVEL_DBFS) < 0.5 and np.abs(x).max() <= 0.98,
          f"levelling hits the target with peaks under full scale ({C.active_rms_db(x):.2f} dBFS)")


def t_storage(tmp):
    print("14 storage")
    import re
    import halo_calib as H
    _, v, err = H.parse_counts(["k", "WR 48000 16000 [string \"x\"]: error writing to file: -28"], "WR", 2)
    check(v == [48000, 16000] and err.endswith("-28"), f"WR reply parsed ({v}, {err!r})")
    _, v, err = H.parse_counts(["SP 56000 nil"], "SP", 1)
    check(v == [56000] and err is None, "SP reply parsed, nil = no error")

    def raises(fn, *args):
        try:
            fn(*args)
        except BaseException as e:      # noqa: BLE001
            return e
        return None
    check(raises(H.check_write, 48000, 48000, None) is None, "a complete write passes")
    e = raises(H.check_write, 48000, 16000, "error writing to file: -28")
    check(isinstance(e, H.StorageFull) and "16 KB free, need 55 KB" in str(e),
          f"ENOSPC on the write -> StorageFull ({e})")
    e = raises(H.check_write, 48000, 16000, "incomplete write: 100 of 4080 bytes")
    check(isinstance(e, H.StorageFull), "a short write -> StorageFull")
    e = raises(H.check_write, 48000, 16000, "error writing to file: -5")
    check(isinstance(e, RuntimeError), "another write error is an ordinary (retried) failure")
    check(raises(H.check_space, 56000, 56000, None) is None
          and isinstance(raises(H.check_space, 12000, 56000, "-28"), H.StorageFull),
          "the probe passes with room and stops without")
    # the developer's case: their app had left ~14 KB free
    r = sim(tmp, "--sim-lfs-free", "14", "--steps", "1")
    sd = latest(tmp)
    s = json.load(open(os.path.join(sd, "session.json")))
    log = open(os.path.join(sd, "log.txt")).read()
    check(r.returncode == 2 and re.search(r"stopped: device storage full: 12 KB free, need \d+ KB; "
                                          r"free space or remove files", r.stdout)
          and "--resume latest" in r.stdout,
          f"probe: stops with the storage message ({r.returncode}) {r.stdout[-300:]}")
    check(not any(t["status"] == "done" for t in s["plan"]) and "attempt 2" not in log
          and "device storage full" in log, "probe: nothing recorded, no BLE retries, logged")
    r = sim(tmp, "--sim-fail", "write_capture", "--steps", "1")
    sd = latest(tmp)
    s = json.load(open(os.path.join(sd, "session.json")))
    log = open(os.path.join(sd, "log.txt")).read()
    check(r.returncode == 2 and "stopped: device storage full:" in r.stdout and "-28" in r.stdout
          and not any(t["status"] == "done" for t in s["plan"]) and "attempt 2" not in log,
          f"write: ENOSPC on cap.lc3 stops the session, no retries ({r.returncode})")
    r = sim(tmp, "--sim-lfs-free", "100", "--steps", "1")
    s = json.load(open(os.path.join(latest(tmp), "session.json")))
    check(r.returncode == 0 and any(t["status"] == "done" for t in s["plan"]),
          f"with 100 KB free step 1 records ({r.returncode})")


def t_saved_gain(tmp):
    print("15 saved-gain")
    for gen in ("old", "scale", "new"):
        for pol in ("keep", "set"):
            steps = "1,2a" if gen == "old" else "1"
            r = sim(tmp, "--sim-gain-gen", gen, "--mic-gain", "3", "--gain-policy", pol,
                    "--steps", steps, "--echo-reps", "1")
            s = json.load(open(os.path.join(latest(tmp), "session.json")))
            dv = s["device"]
            probe = any(t["kind"] == "probe" for t in s["plan"])
            check(r.returncode == 0 and dv.get("gain_effective") == 3
                  and "overrides start" not in r.stdout and "gain_policy" not in dv
                  and "gain_restore" not in dv and not dv.get("gain_restored")
                  and probe == (gen == "old"),
                  f"{gen} firmware, nothing saved, --gain-policy {pol}: gain() reads "
                  f"{dv.get('gain_saved')}, effective {dv.get('gain_effective')}, probe recording "
                  f"{probe}, scale readback {dv.get('gain_scale_probe')} ({r.returncode})")
    # a saved gain equal to the new firmware's default is a saved gain
    for pol, eff in (("keep", 1), ("set", 3)):
        r = sim(tmp, "--sim-gain-gen", "new", "--sim-saved-gain", "1", "--mic-gain", "3",
                "--gain-policy", pol, "--steps", "1")
        dv = json.load(open(os.path.join(latest(tmp), "session.json")))["device"]
        check(r.returncode == 0 and dv.get("gain_effective") == eff and "overrides start" in r.stdout
              and dv.get("gain_policy") == pol and bool(dv.get("gain_restored")) == (pol == "set"),
              f"new firmware, saved 1 (= its default), --gain-policy {pol}: effective "
              f"{dv.get('gain_effective')}, restored {dv.get('gain_restored')} ({r.returncode})")


def main():
    tmp = tempfile.mkdtemp(prefix="calib_test_")
    try:
        C.ensure_clips()
        t_sets()
        t_replay(tmp)
        t_guard(tmp)
        t_ctrl_c(tmp)
        t_vs_current(tmp)
        t_gain_scale(tmp)
        t_storage(tmp)
        t_saved_gain(tmp)
        if "--quick" not in sys.argv:
            full = t_sim_resume(tmp)
            notune = t_sim_notune(tmp)
            t_tagless(tmp, copy_session(tmp, notune, "tagless"))
            t_align(tmp, full)
            t_step4_set(tmp, full)
            t_resume_checks(tmp, full, notune)
            t_restore(tmp)
            t_step4_wins(tmp, full)
    finally:
        if not FAILS:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print(f"(kept {tmp})")
    print(f"\n{'ALL PASS' if not FAILS else str(len(FAILS)) + ' FAIL'}")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
