"""Shared pieces of the worn AEC calibration harness (tests/aec/README.md,
"Worn calibration").

Audio helpers, LC3, the reply clips and wearer phrases (generated with
macOS `say`), the aec_tune parameter sets, and the session store
(incremental, resumable).

Parameter sets are frame.microphone.aec_tune tables throughout ({key:
value}, the device's own key names and units). The replay runs them through
the same C API (calib_build.py, aec_replay -t), so a set means the same
thing offline and on the device.
"""
import json
import os
import re
import shutil
import subprocess
import tempfile
import time
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SR = 16000
HOP = SR // 50                       # 20 ms analysis frames
BITRATE = 32000
FRAME_MS = 10
LC3_FRAME_BYTES = BITRATE // 8 * FRAME_MS // 1000   # 40
LC3_BPS = BITRATE // 8               # capture bytes per second (4000)
BANDS = {"full": (300, 3400), "lo": (300, 800), "mid": (800, 1600), "hi": (1600, 3400)}
AEC_DELAY = 320 + 128                # device AEC output latency (hold-back + kernel)
CLIPS_DIR = os.environ.get("AEC_CALIB_CLIPS", os.path.join(HERE, "clips"))

# --------------------------------------------------------------------- audio


def read_wav(path):
    """int16 mono 16 kHz -> float64 in [-1, 1)."""
    with wave.open(path) as w:
        assert w.getframerate() == SR and w.getnchannels() == 1, f"{path}: need 16 kHz mono"
        return np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64) / 32768


def write_wav(path, x):
    x = np.asarray(x)
    if x.dtype != np.int16:
        x = np.clip(np.round(x * 32768), -32768, 32767).astype(np.int16)
    tmp = path + ".part"
    with wave.open(tmp, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(x.tobytes())
    os.replace(tmp, path)


def bandpass(x, lo, hi):
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1.0 / SR)
    X[(f < lo) | (f > hi)] = 0
    return np.fft.irfft(X, n=len(x))


def db(p):
    return 10 * np.log10(np.asarray(p, dtype=float) + 1e-15)


def fe(x, band="full", n=None):
    """20 ms frame energies in a band (n frames, zero padded)."""
    lo, hi = BANDS[band]
    m = len(x) // HOP if n is None else n
    xb = bandpass(np.asarray(x, float), lo, hi)
    xb = np.pad(xb, (0, max(0, m * HOP - len(xb))))[:m * HOP]
    return (xb.reshape(m, HOP) ** 2).mean(axis=1)


def shift(x, d):
    """y[i] = x[i + d], zero padded."""
    y = np.zeros_like(x)
    if d >= 0:
        y[:len(x) - d] = x[d:]
    else:
        y[-d:] = x[:len(x) + d]
    return y


def xcorr_lag(cap, ref, lo=300, hi=3400):
    """Lag (samples) of ref inside cap, and a peak-to-median quality score."""
    a = bandpass(np.asarray(cap, float), lo, hi)
    b = bandpass(np.asarray(ref, float), lo, hi)
    n = 1 << int(np.ceil(np.log2(len(a) + len(b))))
    c = np.fft.irfft(np.fft.rfft(a, n) * np.conj(np.fft.rfft(b, n)), n)
    c = np.abs(c[:len(a)])
    k = int(np.argmax(c))
    return k, float(c[k] / (np.median(c) + 1e-12))


def find_pauses(x, min_ms=150, rel_db=-35):
    """Gaps >= min_ms inside a clip (20 ms frames, rel_db re the loudest)."""
    e = fe(x)
    act = e > e.max() * 10 ** (rel_db / 10)
    out, k, n = [], 0, len(e)
    while k < n:
        if not act[k]:
            j = k
            while j < n and not act[j]:
                j += 1
            if (j - k) * 20 >= min_ms and k > 0 and j < n:
                out.append((round(k * 0.02, 2), round(j * 0.02, 2)))
            k = j
        else:
            k += 1
    return out


def active_rms_db(x):
    """Speech-band level over frames within 30 dB of the loudest (dBFS)."""
    e = fe(x)
    return float(db(e[e > e.max() * 1e-3].mean()))

# ----------------------------------------------------------------------- LC3


def lc3_encode(pcm_f):
    import lc3
    pcm = np.clip(np.round(np.asarray(pcm_f) * 32768), -32768, 32767).astype(np.int16)
    spf = SR * FRAME_MS // 1000
    pcm = pcm[:len(pcm) - len(pcm) % spf]
    enc = lc3.Encoder(FRAME_MS * 1000, SR, 1)
    return b"".join(enc.encode(pcm[i:i + spf].tobytes(), LC3_FRAME_BYTES, bit_depth=16)
                    for i in range(0, len(pcm), spf))


def lc3_decode(data):
    import lc3
    dec = lc3.Decoder(FRAME_MS * 1000, SR, 1)
    data = bytes(data)
    out = [np.frombuffer(dec.decode(data[i:i + LC3_FRAME_BYTES], bit_depth=16), dtype=np.int16)
           for i in range(0, len(data) - LC3_FRAME_BYTES + 1, LC3_FRAME_BYTES)]
    return np.concatenate(out).astype(np.float64) / 32768 if out else np.zeros(0)

# ------------------------------------------------------------ clips/phrases

# Assistant replies played by the device: multi-second, with 150-700 ms
# pauses between sentences ([[slnc N]]), like a Noa reply. All three are
# levelled to REPLY_LEVEL_DBFS (speech band), the level of a real Noa TTS
# reply as uploaded, because the echo coupling and the speaker_protect
# limiter depend on it.
SAY_REPLIES = {
    "A": ("Karen",
          "Here's the latest. [[slnc 600]] The council has approved the new bike lanes "
          "along the river, [[slnc 250]] and work starts next month. [[slnc 450]] "
          "Traffic on the bridge will be reduced to one lane, [[slnc 180]] so expect delays "
          "in the morning. [[slnc 640]] Do you want the full article?"),
    "B": ("Samantha",
          "Sure. [[slnc 450]] Tomorrow looks mostly sunny, with a high of twenty three degrees "
          "and a light breeze from the west. [[slnc 300]] There is a small chance of rain "
          "in the late afternoon, [[slnc 180]] so you might want to carry an umbrella. "
          "[[slnc 650]] Would you like me to set a reminder?"),
    "C": ("Moira",
          "Okay, [[slnc 220]] here's what I found. [[slnc 500]] The museum opens at nine "
          "thirty on weekdays and ten on weekends. [[slnc 350]] Tickets are eighteen dollars "
          "for adults, [[slnc 160]] and children under twelve get in free. [[slnc 700]] "
          "The nearest station is about a five minute walk away."),
}
REPLY_LEVEL_DBFS = -22.8
CLIP_SECONDS = 10.0          # cap; Lua RAM holds one clip (40 kB of LC3 at 10 s)

# Wearer phrases (step 2b reading, step 2c interruptions)
READ_PHRASES = [
    "What's the weather like tomorrow?",
    "Remind me to call my sister at six.",
    "How far is it to the train station?",
    "Play something relaxing, please.",
    "Can you read that message again?",
    "Add milk and eggs to my shopping list.",
    "What time does the pharmacy close?",
    "Turn the volume down a little.",
    "Who wrote that book?",
    "Set a timer for ten minutes.",
    "Is there a coffee shop nearby?",
    "Translate thank you into French.",
    "Send a text to Alex saying I'm late.",
    "What's on my calendar this afternoon?",
    "Never mind, that's all for now.",
    "How do you spell necessary?",
]
INTERRUPT_PHRASES = [
    "Wait, stop.",
    "Hold on, what about Saturday?",
    "Hey, sorry, quick question.",
    "No, cancel that.",
    "Stop, that's enough.",
    "Actually, make it tomorrow.",
]
DESK_VOICE = "Daniel"        # the Mac "wearer" in --desk mode (male, unlike the replies)
# --desk phrases are levelled to the speech level (active_rms_db) of the
# desk_aec.py talker clip (voice_a.wav, -18.6 dBFS), so afplay -v 1.0 plays
# them at that harness's realistic talker level (its voice reaches -25.6 dBFS
# at Halo 28, -26.9 at Halo EC on the desk). The raw `say` phrases (-6 dBFS
# peak) sat 6-9 dB lower. The lower male voice still arrives ~3.5 dB under
# voice_a at the mic (Halo EC: -30.1 vs -26.9 dBFS, mostly 300-800 Hz, the Mac
# speaker's response); a 3 dB hotter clip gained only 1 dB there.
DESK_VOICE_LEVEL_DBFS = -18.6
DESK_VOICE_PEAK = 0.98


def _say(voice, text):
    if not shutil.which("say"):
        raise SystemExit("the reply clips are generated with macOS `say`: generate clips/ on a "
                         "Mac once and copy it here (or point AEC_CALIB_CLIPS at a copy)")
    with tempfile.TemporaryDirectory() as td:
        p = os.path.join(td, "s.wav")
        cmd = ["say", "-o", p, "--data-format=LEI16@16000", text]
        r = subprocess.run(["say", "-v", voice] + cmd[1:], capture_output=True)
        if r.returncode:            # voice not installed: the system voice
            subprocess.run(cmd, check=True)
        return read_wav(p)


def _trim(x, pad_s=0.05):
    e = fe(x)
    act = np.nonzero(e > e.max() * 10 ** -4.5)[0]
    if not len(act):
        return x
    a = max(0, act[0] * HOP - int(pad_s * SR))
    b = min(len(x), (act[-1] + 1) * HOP + int(pad_s * SR))
    return x[a:b]


def _frame_len(x):
    spf = SR * FRAME_MS // 1000
    return x[:len(x) - len(x) % spf]


def ensure_clips(d=None):
    """Reply clips (PCM + LC3 bytes + meta) in d/. Generated once."""
    d = d or os.path.join(CLIPS_DIR, "replies")
    os.makedirs(d, exist_ok=True)
    meta_p = os.path.join(d, "clips.json")
    if os.path.exists(meta_p):
        return json.load(open(meta_p))
    clips, pcm = {}, {}
    for cid, (voice, text) in SAY_REPLIES.items():
        x = _trim(_say(voice, text))[:int(CLIP_SECONDS * SR)]
        g = 10 ** ((REPLY_LEVEL_DBFS - active_rms_db(x)) / 20)
        x = x * g
        pk = np.abs(x).max()
        if pk > 0.98:
            x *= 0.98 / pk
        pcm[cid] = _frame_len(x)
        clips[cid] = dict(src="say", voice=voice, text=text, gain_db=round(20 * np.log10(g), 2))
    for cid, x in pcm.items():
        blob = lc3_encode(x)
        write_wav(os.path.join(d, f"{cid}.wav"), x)
        open(os.path.join(d, f"{cid}.lc3"), "wb").write(blob)
        clips[cid].update(seconds=round(len(x) / SR, 3), lc3_bytes=len(blob),
                          level_dbfs=round(active_rms_db(x), 1), pauses=find_pauses(x))
    json.dump(clips, open(meta_p, "w"), indent=1)
    return clips


def _peak_limit(x, ceil=0.98, release_s=0.05):
    """Gain-reduction limiter (instant attack, ~2 ms look-ahead, exponential
    release) so peaks stay under ceil; speech level moves < 0.5 dB."""
    x = np.asarray(x, float)
    need = np.minimum(1.0, ceil / np.maximum(np.abs(x), 1e-12))
    la = int(0.002 * SR)
    # look-ahead: the gain reaches its minimum before the peak
    need = np.array([need[max(0, i - la):i + la + 1].min() for i in range(len(need))]) \
        if need.min() < 1.0 else need
    g = np.empty_like(need)
    a = np.exp(-1.0 / (release_s * SR))
    cur = 1.0
    for i, n in enumerate(need):
        cur = n if n < cur else n + (cur - n) * a
        g[i] = cur
    return x * g


def level_desk_phrase(x, target=None):
    """A `say` phrase at the desk talker's speech level (DESK_VOICE_LEVEL_DBFS),
    peaks limited to DESK_VOICE_PEAK."""
    target = DESK_VOICE_LEVEL_DBFS if target is None else target
    for _ in range(8):
        x = x * 10 ** ((target - active_rms_db(x)) / 20)
        if np.abs(x).max() <= DESK_VOICE_PEAK:
            break
        x = _peak_limit(x, DESK_VOICE_PEAK * 0.99)
    return np.clip(x, -DESK_VOICE_PEAK, DESK_VOICE_PEAK)


def ensure_desk_voice(d=None):
    """Pre-generated `say` phrases for --desk mode (the Mac plays the wearer),
    levelled to DESK_VOICE_LEVEL_DBFS. Clips made at another level (older
    trees: -6 dBFS peak) are made again."""
    d = d or os.path.join(CLIPS_DIR, "desk_voice")
    os.makedirs(d, exist_ok=True)
    meta_p = os.path.join(d, "level.json")
    try:
        lvl = json.load(open(meta_p)).get("level_dbfs")
    except (OSError, ValueError):
        lvl = None
    fresh = (lvl is None or abs(lvl - DESK_VOICE_LEVEL_DBFS) > 0.05
             or json.load(open(meta_p)).get("peak") != DESK_VOICE_PEAK)
    out = {}
    for i, txt in enumerate(READ_PHRASES + INTERRUPT_PHRASES):
        key = ("r" if i < len(READ_PHRASES) else "i") + str(i if i < len(READ_PHRASES)
                                                           else i - len(READ_PHRASES))
        p = os.path.join(d, f"{key}.wav")
        if fresh or not os.path.exists(p):
            write_wav(p, level_desk_phrase(_trim(_say(DESK_VOICE, txt))))
        out[txt] = p
    if fresh:
        json.dump({"level_dbfs": DESK_VOICE_LEVEL_DBFS, "peak": DESK_VOICE_PEAK, "voice": DESK_VOICE},
                  open(meta_p, "w"))
    return out

# ------------------------------------------------------- aec_tune sets

# The barge-in keys the named sets and the search touch. Every named set
# gives all of them, so it means the same thing whatever the firmware's
# compiled defaults are.
SET_KEYS = ("sup_beta", "sup_floor", "steady_gcap", "gate_kappa", "gate_absfloor",
            "gate_hang_ms", "gate_band_hz", "cap_split_hz", "cap_lo_gcap")
_A15 = dict(sup_beta=1.25, sup_floor=0.15, steady_gcap=0.25, gate_kappa=0.47, gate_absfloor=0.9,
            gate_hang_ms=1200, gate_band_hz=0, cap_split_hz=0, cap_lo_gcap=0.5)
NAMED = {
    # the defaults before the gate band: full-band gate, falsely released by
    # loud echo on high-coupling units
    "old-gate": dict(sup_beta=1.5, sup_floor=0.1, steady_gcap=0.25, gate_kappa=0.15,
                   gate_absfloor=0.5, gate_hang_ms=1000, gate_band_hz=0, cap_split_hz=0,
                   cap_lo_gcap=0.5),
    # offline-tuned sets; B2-15 is the compiled default since the gate band
    "B2-15": dict(_A15, gate_kappa=0.5, gate_band_hz=1000, cap_split_hz=750, cap_lo_gcap=0.5),
    "B15": dict(_A15, gate_kappa=0.5, gate_band_hz=1000),
    "A15": dict(_A15),
}
REF_SET = "0.8.17"           # the 0.8.17 release source (calib_build.aec_replay_ref)


def norm(v):
    return round(float(v), 6)


def same(a, b):
    return abs(float(a) - float(b)) <= 1e-4 * max(1.0, abs(float(b)))


def ms_round(k, v):
    """The device truncates ms keys to 20 ms."""
    return (int(v) // 20) * 20 if k.endswith("_ms") else v


def apply(base, over):
    """A full table: base (the device's) with over's keys (unknown keys dropped)."""
    t = dict(base)
    t.update({k: ms_round(k, v) for k, v in (over or {}).items() if k in base})
    return t


def diff(table, base):
    """Keys of table that differ from base (what aec_tune{...} has to send)."""
    return {k: v for k, v in table.items() if k in base and not same(v, base[k])}


def missing(over, keys):
    """Keys of a set the firmware does not have."""
    return sorted(k for k in (over or {}) if keys is not None and k not in keys)


def fmt_val(v):
    v = float(v)
    return str(int(v)) if v == int(v) else f"{v:g}"


def lua_tune_line(table, defaults):
    """Ready-to-paste REPL line for a set: aec_tune('defaults') first (aec_tune{}
    changes only the keys it names, on top of whatever set is live), then
    aec_tune{...} with the keys that differ from the defaults. One line: a
    newline ends a REPL command."""
    d = diff(table, defaults)
    line = "frame.microphone.aec_tune('defaults')"
    if d:
        line += (" frame.microphone.aec_tune{"
                 + ", ".join(f"{k}={fmt_val(v)}" for k, v in sorted(d.items())) + "}")
    return line


_MACROS = None


def tune_macros():
    """aec_tune key -> (compile-time macro in audio_aec.c, unit divisor), read
    from the source's AEC_TUNE_DEFAULTS initializer."""
    global _MACROS
    if _MACROS is None:
        import calib_build
        src = open(calib_build.AEC_SRC).read()
        blk = src[src.index("#define AEC_TUNE_DEFAULTS"):]
        blk = blk[:blk.index("}")]
        _MACROS = {m.group(1): (m.group(2), int(m.group(3) or 1)) for m in
                   re.finditer(r"\.(\w+) = (AEC_\w+)(?: \* (\d+))?", blk)}
    return _MACROS


def c_defines(table, defaults):
    out = []
    for k, v in sorted(diff(table, defaults).items()):
        mac, div = tune_macros().get(k, (None, 1))
        if mac is None:
            out.append(f"/* {k} = {fmt_val(v)} (no compile-time macro found) */")
        elif div != 1 or float(v) == int(float(v)) and k.endswith(("_ms", "_hz", "_lift")):
            out.append(f"#define {mac} {int(float(v)) // div}")
        else:
            out.append(f"#define {mac} {float(v)!r}f")
    return "\n".join(out)


def tune_mismatch(want, got):
    """Keys whose aec_tune() readback differs from the request (ms keys
    round down to 20 ms; floats are float32 on the device)."""
    bad = {}
    for k, v in want.items():
        g = (got or {}).get(k)
        if g is None or not same(g, ms_round(k, v)):
            bad[k] = (v, g)
    return bad

# ------------------------------------------------------------------ session


class Session:
    """One calibration sitting: session.json (settings, device facts, plan
    with per-trial status) plus trials/<id>.{wav,lc3,json}. Every write is
    atomic, so a crash or BLE drop never loses a finished trial."""

    def __init__(self, root):
        self.root = root
        self.p = os.path.join(root, "session.json")
        for sub in ("trials", "clips", "results"):
            os.makedirs(os.path.join(root, sub), exist_ok=True)
        self.s = json.load(open(self.p)) if os.path.exists(self.p) else {}

    def save(self):
        tmp = self.p + ".part"
        with open(tmp, "w") as f:
            json.dump(self.s, f, indent=1)
        os.replace(tmp, self.p)

    def log(self, msg):
        with open(os.path.join(self.root, "log.txt"), "a") as f:
            f.write(time.strftime("%H:%M:%S ") + msg + "\n")

    def trial_path(self, tid, ext):
        return os.path.join(self.root, "trials", f"{tid}.{ext}")

    def trials(self, step=None, kind=None, done=None):
        out = []
        for t in self.s.get("plan", []):
            if step and t["step"] != step:
                continue
            if kind and t["kind"] != kind:
                continue
            if done is not None and (t.get("status") == "done") != done:
                continue
            out.append(t)
        return out

    def save_trial(self, t, pcm, raw, rec):
        write_wav(self.trial_path(t["id"], "wav"), pcm)
        if raw is not None:
            open(self.trial_path(t["id"], "lc3"), "wb").write(raw)
        tmp = self.trial_path(t["id"], "json.part")
        with open(tmp, "w") as f:
            json.dump(dict(t, **rec), f)
        os.replace(tmp, self.trial_path(t["id"], "json"))
        t["status"] = "done"
        t["finished"] = time.strftime("%H:%M:%S")
        self.save()

    def load_trial(self, tid):
        rec = json.load(open(self.trial_path(tid, "json")))
        rec["x"] = read_wav(self.trial_path(tid, "wav"))
        return rec

    def defaults(self):
        """The device's aec_tune defaults (None: firmware without aec_tune)."""
        dv = self.s.get("device", {})
        return dv.get("aec_defaults") or dv.get("aec_tune")

    def live(self):
        return self.s.get("device", {}).get("aec_tune")


def copy_clips(src, dst):
    os.makedirs(dst, exist_ok=True)
    for f in os.listdir(src):
        shutil.copy2(os.path.join(src, f), os.path.join(dst, f))
