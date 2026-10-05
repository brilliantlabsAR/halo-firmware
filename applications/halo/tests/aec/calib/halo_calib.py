#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10,<3.13"
# dependencies = [
#     "brilliant-ble>=3.3.0,<4",
#     "lc3py",
#     "numpy",
# ]
# ///
"""
Worn AEC calibration for Halo. One sitting, ~13 minutes.

The wearer sits at this Mac wearing the Halo; the terminal shows every prompt
with a countdown. Steps:
  1   setup: firmware, aec_tune table, saved mic gain; noise floor (5 s)
  2a  echo only: the device plays three TTS replies (x2), wearer silent  (AEC off)
  2b  wearer only: read short phrases; normal (x2), quieter, louder       (AEC off)
  2c  wearer over echo: interrupt a playing reply on cue                  (AEC off)
  3   offline optimisation on these captures (real firmware AEC replay)
  4   AEC on, A/B: device defaults vs the recommended set (aec_tune)
  report.md with the tables; a ready-to-paste aec_tune{...} line only for a
  set that passed step 4 (else the headline is "keep the current defaults")

Everything is saved as it is captured (sessions/<stamp>-<device>/). A BLE
drop retries the current step (not the session); if retries run out, or on
Ctrl+C, rerun with --resume to continue where it stopped.

Usage:
  uv run halo_calib.py --name "Halo 28"                 worn run (prompts the wearer)
  uv run halo_calib.py --name "Halo EC" --desk          desk dry run: the Mac plays the wearer
  uv run halo_calib.py --resume latest                  continue the last session
  uv run halo_calib.py --resume sessions/<dir> --redo 2a,4
  uv run halo_calib.py --sim --fast --desk              no device: simulated Halo (tests)

Step 4 runs only on firmware with frame.microphone.aec_tune; steps 1-3 work
on any firmware with start{aec=, voice=} (0.8.17 and later). Step 3 replays
the captures through this tree's audio_aec.c (built on the host on first
use: needs a C compiler), so run it from the tree the device was built from.
Nothing on the device is wiped. /lfs/cap.lc3 is written and removed. If a
saved mic gain (frame.microphone.gain(), the persisted audio/gain setting)
overrides start{gain=} and you agree (or pass --gain-policy set), it is set
for the sitting and the old value is saved in session.json first and
restored at the end. aec_tune is put back to its defaults and the VM is reset
at the end so main.lua resumes; anything that could not be restored is
listed with the command that fixes it.

--resume checks it is the same device (--name), the same firmware and the
same mic gain choice as the session; a firmware change needs --redo of the
recorded steps.
"""

import argparse
import asyncio
import json
import os
import shutil
import subprocess
import sys
import threading
import time
from datetime import datetime

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import calib_common as C  # noqa: E402

HERE = C.HERE
LEAD_S = 1.0
TAIL_S = 1.0
CHUNK_FRAMES = 6                 # 60 ms of LC3 per speaker.play()
DIAG_FIELDS = ["sup_gate_rel", "sup_onset", "p_mic", "p_ref", "p_err", "sup_gmean",
               "w_norm2", "resyncs", "ref_underruns", "sup_pb_hold", "tune_gen", "ref_quiet"]
# (fields a firmware lacks read -1: tune_gen/ref_quiet arrived with aec_tune)
STEPS = ["1", "2a", "2b", "2c", "3", "4"]
STEP_TITLES = {
    "1": "Step 1 of 4: setup and noise floor",
    "2a": "Step 2a: echo only (stay silent)",
    "2b": "Step 2b: your voice only (read the phrases)",
    "2c": "Step 2c: interrupt the assistant",
    "3": "Step 3: optimising (host only)",
    "4": "Step 4: AEC on, A/B check",
}
STEP_HELP = {
    "1": ["Sit comfortably, glasses on as you would wear them.",
          "We measure the room's noise floor for 5 seconds: please stay silent."],
    "2a": ["The glasses will play several assistant replies.",
           "Stay completely silent and still while they play (about 10 s each)."],
    "2b": ["No audio from the glasses now. Read each phrase aloud when it says NOW.",
           "Normal voice first; later one quieter and one louder pass."],
    "2c": ["The glasses play a reply. When the screen says NOW, interrupt it with",
           "the phrase shown, as if cutting off an assistant. The reply keeps playing:",
           "just say the phrase once and then stay quiet."],
    "3": ["Keep the glasses on. The Mac replays your captures through the firmware",
          "echo canceller to pick the best settings. About 1-2 minutes. Relax."],
    "4": ["Same as before, now with echo cancellation on, two settings A/B.",
          "Stay silent during the first reply of each pair; interrupt on cue in the others."],
}
LEVEL_TEXT = {"normal": "your normal voice", "quiet": "QUIETER than normal",
              "loud": "LOUDER than normal (not shouting)"}

# ---------------------------------------------------------------------- plan


def build_plan(a):
    P = []

    def add(tid, step, kind, **kw):
        t = dict(id=tid, step=step, kind=kind, aec=False, clip=None, prompts=[], seconds=None,
                 level=None, tune=None, status="todo")
        t.update(kw)
        P.append(t)

    add("1-floor", "1", "floor", seconds=5.0)
    for cid in ("A", "B", "C"):
        for rep in range(1, a.echo_reps + 1):
            add(f"2a-echo-{cid}-r{rep}", "2a", "echo", clip=cid)
    ph = list(C.READ_PHRASES)
    passes = [("normal", 1), ("normal", 2), ("quiet", 1), ("loud", 1)]
    for i, (lvl, rep) in enumerate(passes):
        at = [2.5, 5.5, 8.5, 11.5]
        add(f"2b-wearer-{lvl}-r{rep}", "2b", "wearer", level=lvl, seconds=14.0,
            prompts=[dict(at=at[j], text=ph[(4 * i + j) % len(ph)], level=lvl) for j in range(4)])
    ip = C.INTERRUPT_PHRASES
    dt_at = {"A": (0.6, 5.5), "B": (2.5, 7.0), "C": (1.5, 6.0)}
    for i, cid in enumerate(("A", "B", "C")):
        add(f"2c-dt-{cid}", "2c", "dt", clip=cid, level="normal",
            prompts=[dict(at=dt_at[cid][j], text=ip[(2 * i + j) % len(ip)], level="normal")
                     for j in range(2)])
    # step 4: ABBA order (cur, rec, rec, cur) so drift cancels in the A/B
    clips4 = ["B", "C"][:a.confirm_reps]
    for r, cid in enumerate(clips4):
        order = ("cur", "rec") if r % 2 == 0 else ("rec", "cur")
        for s in order:
            add(f"4-echo-{cid}-{s}", "4", "echo", clip=cid, aec=True, tune=s)
        for j, s in enumerate(order[::-1]):
            add(f"4-dt-{cid}-{s}", "4", "dt", clip=cid, aec=True, tune=s, level="normal",
                prompts=[dict(at=dt_at[cid][k], text=ip[(3 + 2 * j + k + r) % len(ip)], level="normal")
                         for k in range(2)])
    return P


def est_minutes(plan, step3_pending=True):
    """From the desk runs: ~4.5 s of Lua setup + readback per 12 s recording,
    ~10 s per 40 kB clip upload; plus countdowns, step intros and step 3."""
    s, clip, steps = 30.0, None, set()
    for t in plan:
        s += (t["seconds"] or 10.0) + LEAD_S + TAIL_S + 5 + (3 if t["prompts"] else 0)
        if t["clip"] and t["clip"] != clip:
            s += 10
            clip = t["clip"]
        steps.add(t["step"])
    s += 20 * len(steps) + (70 if step3_pending else 0)
    return s / 60


# ------------------------------------------------------------------------ UI


class UI:
    """Terminal prompts. In worn mode it waits for Enter between steps; in
    desk/sim mode it continues by itself. fast=True drops all waits (sim)."""

    B, R, D, X = "\033[1m", "\033[7m", "\033[2m", "\033[0m"

    def __init__(self, a):
        self.worn = not (a.desk or a.sim)
        self.fast = a.fast
        self.tty = sys.stdout.isatty() and not a.fast
        if not self.tty:
            self.B = self.R = self.D = self.X = ""

    def _w(self, s=""):
        print(s, flush=True)

    def clear(self):
        if self.tty:
            print("\033[2J\033[H", end="")

    def header(self, title, lines=()):
        self.clear()
        self._w(f"{self.B}{'=' * 64}\n {title}\n{'=' * 64}{self.X}")
        for ln in lines:
            self._w(" " + ln)
        self._w()

    def info(self, s):
        self._w(" " + s)

    def warn(self, s):
        self._w(f" {self.B}!! {s}{self.X}")

    def big(self, s):
        if self.tty:
            self._w(f"\n   {self.R}{self.B}  {s}  {self.X}\n")
        else:
            self._w(f"   >>> {s}")

    async def sleep(self, s):
        if not self.fast and s > 0:
            await asyncio.sleep(s)

    async def enter(self, msg="Press Enter when ready"):
        if self.worn:
            await ainput(f" {self.B}{msg}{self.X} ")
        else:
            self.info(f"{msg} (auto)")
            await self.sleep(1.5)

    async def ask(self, msg, choices, default):
        if not self.worn:
            return default
        while True:
            r = (await ainput(f" {self.B}{msg}{self.X} ")).strip().lower() or default
            if r[:1] in choices:
                return r[:1]

    async def countdown(self, n, label):
        for k in range(n, 0, -1):
            self.info(f"{label} {k}...")
            await self.sleep(1.0)

    async def prompts(self, prompts, t0, player=None, wearer=True):
        """Show each prompt at t0 + at (host clock): 'get ready' 2 s before,
        then NOW. In desk mode the player speaks the phrase ~0.3 s after NOW."""
        procs = []
        for p in sorted(prompts, key=lambda p: p["at"]):
            ready = t0 + max(0.0, p["at"] - 2.0)
            await self.sleep(ready - time.monotonic())
            self.info(f"{self.D}get ready ({LEVEL_TEXT.get(p.get('level'), '')}):{self.X} {p['text']}")
            await self.sleep(t0 + p["at"] - time.monotonic())
            self.big(f"NOW:  {p['text']}")
            if player is not None:
                procs.append(await player.say(p, delay=0.3))
        for pr in procs:
            if pr is not None:
                try:
                    await asyncio.wait_for(pr.wait(), 8)
                except asyncio.TimeoutError:
                    pr.kill()


_STDIN_BUF = bytearray()


async def ainput(prompt=""):
    """input() that Ctrl+C can cancel. asyncio.to_thread(input) left a worker
    thread blocked in input(), and the interpreter waits for it at exit, so
    a Ctrl+C at a prompt hung after the cleanup until Enter was pressed. This
    waits for stdin in the event loop instead (a daemon thread where the loop
    cannot watch stdin), so a cancel needs no thread to finish."""
    print(prompt, end="", flush=True)
    loop = asyncio.get_running_loop()
    fd = sys.stdin.fileno()
    while b"\n" not in _STDIN_BUF:
        fut = loop.create_future()

        def readable():
            if not fut.done():
                fut.set_result(None)
        try:
            loop.add_reader(fd, readable)
        except (NotImplementedError, ValueError, OSError):
            return await _ainput_thread(loop)
        try:
            await fut
        finally:
            loop.remove_reader(fd)
        chunk = os.read(fd, 4096)
        if not chunk:
            raise EOFError("stdin closed")
        _STDIN_BUF.extend(chunk)
    i = _STDIN_BUF.index(b"\n")
    line = bytes(_STDIN_BUF[:i]).decode(errors="replace")
    del _STDIN_BUF[:i + 1]
    return line


async def _ainput_thread(loop):
    fut = loop.create_future()

    def work():
        try:
            r, e = sys.stdin.readline(), None
        except BaseException as x:      # noqa: BLE001 (handed to the loop)
            r, e = None, x
        def done():
            if not fut.done():
                fut.set_exception(e) if e else fut.set_result(r.rstrip("\n"))
        try:
            loop.call_soon_threadsafe(done)
        except RuntimeError:            # the loop is gone (cancelled and closed)
            pass
    threading.Thread(target=work, daemon=True).start()
    return await fut


class DeskPlayer:
    """--desk: the Mac speaker plays a pre-generated `say` voice for the
    wearer. Software-attenuated with afplay -v (never the system volume)."""

    def __init__(self, a):
        if not shutil.which("afplay"):
            raise SystemExit("--desk plays the wearer's phrases with macOS afplay")
        self.vol = a.afplay_volume
        self.max = min(1.0, a.afplay_max if a.afplay_max is not None else a.afplay_volume)
        self.wavs = C.ensure_desk_voice()

    def volume(self, level):
        """afplay -v per pass: quiet x0.5, loud x2 but never above --afplay-max
        (default: the normal level, so a night-time run stays at it)."""
        return {"normal": min(self.vol, self.max), "quiet": self.vol * 0.5,
                "loud": min(self.max, self.vol * 2)}[level or "normal"]

    async def say(self, p, delay=0.3):
        await asyncio.sleep(delay)
        return await asyncio.create_subprocess_exec(
            "afplay", "-v", f"{self.volume(p.get('level')):.3f}", self.wavs[p["text"]])

# ---------------------------------------------------------------------- BLE


class Ble:
    """Halo over BLE: brilliant-ble REPL + data channel (desk_aec.py plumbing)."""

    def __init__(self, name, log):
        self.name, self.log = name, log
        self.rx, self.printed = bytearray(), []
        self.b = None
        self.clip_loaded = None

    async def connect(self, tries=8):
        from brilliant_ble import BrilliantBle
        for t in range(tries):
            try:
                self.b = BrilliantBle()
                await self.b.connect(name=self.name, data_response_handler=self.rx.extend)
                self.b._user_print_response_handler = self.printed.append
                self.clip_loaded = None
                return
            except Exception as e:      # 0x98 drops are common: retry
                self.log(f"connect failed ({e!r}); retry {t + 1}/{tries}")
                await asyncio.sleep(min(2 + t, 6))
        raise RuntimeError("could not connect")

    async def disconnect(self):
        try:
            if self.b is not None:
                await self.b.disconnect()
        except Exception:
            pass

    async def ready(self):
        await self.b.send_break_signal()
        await asyncio.sleep(0.5)
        await self.lua("pcall(frame.speaker.stop) pcall(frame.microphone.stop) print('rdy')")

    async def reconnect(self):
        await self.disconnect()
        await asyncio.sleep(2)
        await self.connect()
        await self.ready()

    async def lua(self, s, timeout=10, wait=True):
        await self.b.send_lua(s, await_print=wait, timeout=timeout)

    async def wait_for(self, token, timeout=30):
        t0 = time.monotonic()
        while token not in self.printed:
            if time.monotonic() - t0 > timeout:
                raise RuntimeError(f"timeout waiting for {token}")
            await asyncio.sleep(0.05)

    async def lines(self, cmd, end, timeout=30):
        self.printed.clear()
        await self.lua(cmd, wait=False)
        await self.wait_for(end, timeout)
        return [p for p in self.printed if p != end]

    async def fw(self):
        return (await self.lines("print(frame.FIRMWARE_VERSION) print('FWEND')", "FWEND"))[-1]

    async def tune_table(self):
        """frame.microphone.aec_tune() as a dict, or None if the firmware has none."""
        return (await self.tune_tables(defaults=False))[0]

    async def tune_tables(self, defaults=True):
        """(live aec_tune() table, its defaults) or (None, None) without aec_tune.
        The defaults are read with aec_tune('defaults'), then the live set is
        put back."""
        dflt = ("m.aec_tune('defaults') dump('TD',m.aec_tune()) pcall(m.aec_tune,t) " if defaults else "")
        ls = await self.lines(
            "local m=frame.microphone if not m.aec_tune then print('TT:none') else "
            "local function dump(p,t) local o={} for k,_ in pairs(t) do o[#o+1]=k end table.sort(o) "
            "for i=1,#o do print(p..' '..o[i]..' '..tostring(t[o[i]])) end end "
            "local t=m.aec_tune() dump('TT',t) " + dflt + "end print('TTEND')", "TTEND")
        if any(x == "TT:none" for x in ls):
            return None, None
        out = {"TT": {}, "TD": {}}
        for x in ls:
            if x[:3] in ("TT ", "TD "):
                p, k, v = x.split(" ", 2)
                out[p][k] = C.norm(v) if v not in ("true", "false") else (v == "true")
        return out["TT"], (out["TD"] or None)

    async def tune_apply(self, keys):
        """aec_tune('defaults') then aec_tune{keys} (keys may be empty)."""
        body = ",".join(f"{k}={v}" for k, v in sorted(keys.items()))
        ls = await self.lines(
            "local m=frame.microphone m.aec_tune('defaults') "
            f"local ok,e=pcall(m.aec_tune,{{{body}}}) "
            "print(ok and 'TA:ok' or ('TA:err '..tostring(e))) print('TAEND')", "TAEND")
        r = [x for x in ls if x.startswith("TA:")]
        if not r or r[-1] != "TA:ok":
            raise SystemExit(f"aec_tune rejected the set: {r}")
        return await self.tune_table()

    async def saved_gain(self):
        ls = await self.lines("print('G '..tostring(frame.microphone.gain())) print('GEND')", "GEND")
        return int(float([x for x in ls if x.startswith("G ")][-1][2:]))

    async def set_gain(self, g):
        """gain() persists (audio/gain) and needs a started mic."""
        await self.lua(f"frame.microphone.start{{sample_rate={C.SR}, bit_depth=16, channels=1}} "
                       f"frame.microphone.gain({int(g)}) frame.microphone.stop() print('ok')")

    async def upload(self, cid, blob):
        if self.clip_loaded == cid:
            return
        self.clip_loaded = None
        await self.lua("vc=nil collectgarbage() vc={} vn=0 frame.bluetooth.receive_callback("
                       "function(x) vc[#vc+1]=x vn=vn+#x end) print('rx-ready')")
        chunk = CHUNK_FRAMES * C.LC3_FRAME_BYTES
        for i in range(0, len(blob), chunk):
            await self.b.send_data(bytearray(blob[i:i + chunk]))
        await asyncio.sleep(0.5)
        ls = await self.lines("frame.bluetooth.receive_callback(nil) print(vn) print('UPEND')", "UPEND")
        if int(ls[-1]) != len(blob):
            raise RuntimeError(f"upload incomplete {ls[-1]}/{len(blob)}")
        self.clip_loaded = cid

    async def capture(self, st, aec, tts, seconds, on_start, prompts=()):
        """One trial: mic (and optionally the uploaded clip) on device, captured
        to Lua RAM, written to /lfs/cap.lc3 and read back. Returns
        (pcm, raw lc3, diag rows, info)."""
        target = int((LEAD_S + seconds + TAIL_S) * C.LC3_BPS)
        await self.lua("pcall(frame.file.remove, 'cap.lc3') print('k')")
        await self.lua("capt={} capn=0 dg={} collectgarbage() "
                       "drainf=function() local s=frame.microphone.read(4080) "
                       "if s and s~='' then capt[#capt+1]=s capn=capn+#s end end print('ok2')")
        for i in range(0, len(DIAG_FIELDS), 5):
            names = ",".join(f"'{k}'" for k in DIAG_FIELDS[i:i + 5])
            pre = "dgk={} " if i == 0 else ""
            await self.lua(f"{pre}for _,k in ipairs({{{names}}}) do dgk[#dgk+1]=k end print('k')")
        await self.lua("smp=function() local s=frame.microphone.diag('stats') "
                       "local t={capn} for j=1,#dgk do t[j+1]=tonumber(s[dgk[j]]) or -1 end "
                       "dg[#dg+1]=table.concat(t,' ') end print('k')")
        await self.lua(f"spk=function() frame.speaker.start{{encoder='lc3', sample_rate={C.SR}, "
                       f"channels=1, duration=1000, bitrate={C.BITRATE}, volume={st['volume']}, "
                       f"gain={st['spk_gain']}, budget={st['budget']}}} end print('k')")
        await self.lua(
            f"frame.microphone.start{{encoder='lc3', sample_rate={C.SR}, duration=1000, "
            f"bitrate={C.BITRATE}, gain={st['mic_gain']}, aec={'true' if aec else 'false'}, "
            f"voice=true}} capt={{}} capn=0 "
            f"for i=1,{int(LEAD_S * 100)} do drainf() frame.sleep(0.01) end print('ok3')", timeout=30)
        if tts:
            cmd = ("smp() spk() for i=1,#vc do frame.speaker.play(vc[i]) drainf() smp() end "
                   "frame.speaker.stop() smp() print('ok4')")
        else:
            cmd = (f"smp() for i=1,{int(seconds * 100)} do drainf() frame.sleep(0.01) "
                   "if i%6==0 then smp() end end smp() print('ok4')")
        self.printed.clear()
        t_send = time.monotonic()
        pt = asyncio.ensure_future(on_start(t_send))
        try:
            await self.lua(cmd, timeout=seconds + 60)
            await pt
        except BaseException:
            pt.cancel()
            raise
        if "ok4" not in self.printed:
            raise RuntimeError(f"playback failed: {self.printed[-3:]}")
        self.printed.clear()
        await self.lua(
            f"for i=1,{int((TAIL_S + 5) * 100)} do if capn>={target} then break end "
            "drainf() frame.sleep(0.01) end frame.microphone.stop() "
            "local f=frame.file.open('cap.lc3','w') "
            "for i=1,#capt do pcall(function() f:write(capt[i]) end) end "
            "f:close() capt=nil collectgarbage() print(capn)", timeout=int(TAIL_S) + 60)
        diag = [list(map(float, x.split())) for x in
                await self.lines("for i=1,#dg do print(dg[i]) end print('DGEND')", "DGEND")]
        self.rx.clear()
        await self.lua(
            "local f=frame.file.open('cap.lc3','r') "
            "while true do local s=f:read(240) if s==nil then break end "
            "while true do if(pcall(frame.bluetooth.send,s))then break end end end "
            "f:close() print('sent')", timeout=240)
        await asyncio.sleep(0.5)
        raw = bytes(self.rx)
        raw = raw[:len(raw) - len(raw) % C.LC3_FRAME_BYTES]
        return C.lc3_decode(raw), raw, diag, dict(target_bytes=target, got_bytes=len(raw))

    # end of session (finish_device): one call per thing to put back
    async def tune_defaults(self):
        await self.lua("frame.microphone.aec_tune('defaults') print('k')")

    async def remove_capture(self):
        await self.lua("pcall(frame.file.remove, 'cap.lc3') print('k')")

    async def reset(self):
        await self.b.send_reset_signal()

# ----------------------------------------------------------------------- run


class Abort(BaseException):
    """Stop the session now (state is saved; --resume continues)."""


def gain_cmd(g):
    return (f"frame.microphone.start{{sample_rate={C.SR}, bit_depth=16, channels=1}} "
            f"frame.microphone.gain({int(g)}) frame.microphone.stop()")


async def finish_device(dev, has_tune, restore_gain, root):
    """Put the device back: aec_tune defaults, the saved mic gain, cap.lc3,
    VM reset. Each in its own try, so one failure does not skip the rest.
    Returns [(what, error, how to fix it)] for the ones that failed."""
    failed = []

    async def one(what, fn, fix):
        try:
            await asyncio.wait_for(fn(), 30)
        except Exception as e:          # noqa: BLE001 (reported below)
            failed.append((what, repr(e) if str(e) else type(e).__name__, fix))
    if has_tune:
        await one("aec_tune defaults", dev.tune_defaults,
                  "REPL: frame.microphone.aec_tune('defaults')  (or power-cycle: aec_tune is not "
                  "persisted)")
    if restore_gain is not None:
        await one(f"saved mic gain {restore_gain}", lambda: dev.set_gain(restore_gain),
                  f"uv run halo_calib.py --resume {root} --steps none  (restores it), or REPL: "
                  + gain_cmd(restore_gain))
    await one("/lfs/cap.lc3 removal", dev.remove_capture, "REPL: frame.file.remove('cap.lc3')")
    await one("VM reset (main.lua resumes)", dev.reset, "power-cycle the glasses")
    try:
        await dev.disconnect()
    except Exception:                   # noqa: BLE001
        pass
    return failed


async def run_trial(a, S, dev, ui, t, player, clips_blob, tune_keys):
    st = dict(S.s["settings"])
    if t.get("mic_gain") is not None:       # the saved-gain probe
        st["mic_gain"] = t["mic_gain"]
    tts = t["clip"] is not None
    seconds = S.s["clips"][t["clip"]]["seconds"] if tts else t["seconds"]
    for attempt in range(1, a.retries + 2):
        try:
            if attempt > 1:
                ui.warn(f"BLE dropped; reconnecting (attempt {attempt}/{a.retries + 1}); "
                        "this step restarts from its beginning")
                await dev.reconnect()
            if tts:
                if dev.clip_loaded != t["clip"]:
                    ui.info(f"loading reply {t['clip']} ...")
                await dev.upload(t["clip"], clips_blob[t["clip"]])
            dev_tab = None
            if t["aec"] and tune_keys is not None:
                want = tune_keys[t["tune"]]
                dev_tab = await dev.tune_apply(want)
                bad = C.tune_mismatch(want, dev_tab)
                if bad:
                    S.log(f"trial {t['id']}: aec_tune readback differs: {bad}")
                    ui.warn(f"aec_tune readback differs from the request: {bad}")
            if t["prompts"] and ui.worn:
                await ui.countdown(3, "starting in")
            else:
                await ui.sleep(0.5)
            ui.info("recording" + (" - reply playing" if tts else "") + " ...")

            async def on_start(t0):
                if t["prompts"]:
                    await ui.prompts(t["prompts"], t0, player)
            pcm, raw, diag, info = await dev.capture(st, t["aec"], tts, seconds, on_start,
                                                    prompts=t["prompts"])
            if len(pcm) < 0.8 * (LEAD_S + seconds + TAIL_S) * C.SR:
                raise RuntimeError(f"short capture: {len(pcm) / C.SR:.1f} s")
            rec = dict(diag=diag, diag_fields=["capn"] + DIAG_FIELDS, info=info, attempts=attempt,
                       seconds=seconds, settings=st, fw=S.s["device"].get("fw"),
                       tune_table=dev_tab, stamp=datetime.now().isoformat(timespec="seconds"))
            t["fw"] = S.s["device"].get("fw")    # the plan keeps it: a resume checks it
            S.save_trial(t, pcm, raw, rec)
            S.log(f"trial {t['id']} done (attempt {attempt}, {len(pcm) / C.SR:.1f} s)")
            return pcm
        except (Abort, KeyboardInterrupt):
            raise
        except SystemExit:
            raise
        except Exception as e:
            S.log(f"trial {t['id']} attempt {attempt} failed: {e!r}")
            ui.warn(f"step failed: {e!r}")
            await asyncio.sleep(1.0 if not a.fast else 0)
    raise Abort(f"trial {t['id']} failed {a.retries + 1} times")


def quick_check(S, ui, t, pcm):
    """Per-trial feedback; returns a warning string or None."""
    fl = S.s.get("floor_dbfs")
    e = C.fe(pcm)
    pk = 20 * np.log10(np.abs(pcm).max() + 1e-9)
    msg = None
    if t["kind"] == "floor":
        f = float(C.db(np.median(e[10:int(0.2 * 50) + 250])))
        S.s["floor_dbfs"] = f
        S.save()
        ui.info(f"noise floor {f:.1f} dBFS (300-3400 Hz)")
        return None
    if t["kind"] == "probe":
        gain_probe_verdict(S, ui, pcm)
        return None
    if t["kind"] in ("echo", "dt") and not t["aec"]:
        lvl = float(C.db(np.percentile(e[int(1.2 * 50):-int(1.2 * 50)], 75)))
        ui.info(f"echo level ~{lvl:.1f} dBFS, peak {pk:.1f} dBFS"
                + (f", {lvl - fl:.0f} dB over the floor" if fl is not None else ""))
        if fl is not None and lvl - fl < 15:
            msg = "the echo is weak (< 15 dB over the floor): are the glasses on, volume right?"
        if pk > -0.3:
            msg = "the capture clips (peak at full scale): the mic gain may be above 1"
    if t["kind"] == "wearer":
        f = 10 ** ((fl if fl is not None else -60) / 10)
        act = (e > f * 10)[10:]
        lvl = float(C.db(e[10:][act].mean())) if act.any() else None
        ui.info(f"voice frames: {int(act.sum())} x 20 ms" + (f", level {lvl:.1f} dBFS" if lvl else ""))
        if act.sum() < 30:
            msg = "we barely heard you"
    return msg


async def step3(a, S, ui):
    cmd = [sys.executable, os.path.join(HERE, "calib_offline.py"), "step3", S.root,
           "--budget", str(a.search_budget)]
    if a.no_guard:
        cmd.append("--no-guard")
    elif a.guard:
        cmd += ["--guard"] + a.guard
    ui.info("running: " + " ".join(cmd[1:]))
    p = await asyncio.create_subprocess_exec(*cmd, stdout=asyncio.subprocess.PIPE,
                                             stderr=asyncio.subprocess.STDOUT)
    while True:
        ln = await p.stdout.readline()
        if not ln:
            break
        s = ln.decode(errors="replace").rstrip()
        ui.info(s)
        S.log("step3: " + s)
    rc = await p.wait()
    if rc != 0:
        raise RuntimeError(f"step 3 failed (exit {rc})")
    S.s["step3_done"] = True
    S.save()


def rec_tune_keys(S):
    """Device keys for the step 4 sets: 'cur' = the device defaults, 'rec' =
    step 3's candidate (the keys that differ from the defaults)."""
    p = os.path.join(S.root, "results", "step3.json")
    dflt = S.defaults()
    rec = None
    if S.s.get("step3_done") and os.path.exists(p):     # else it is stale or failed
        r = json.load(open(p))
        if r.get("step4_candidate"):
            rec = r["step4_candidate"]["params"]
    if rec is None:
        rec = C.apply(dflt, C.NAMED["B15"])
        S.log("step 4: no step 3 result; testing B15")
    keys = C.diff(rec, dflt)
    dropped = C.missing(keys, dflt)
    if dropped:     # never send a key the firmware does not know (aec_tune raises)
        S.log(f"step 4: firmware lacks {dropped}; testing without them")
    S.s["step4_dropped_keys"] = dropped
    return {"cur": {}, "rec": {k: v for k, v in keys.items() if k in dflt}}


def recorded_steps(S):
    """Steps with finished recordings (the order of STEPS)."""
    done = {t["step"] for t in S.s.get("plan", []) if t.get("status") == "done"}
    return [s for s in STEPS if s in done]


def check_firmware(S, fw, tab_def):
    """On a resume: recordings made on another firmware (and a step 3 scored
    against other aec_tune defaults) are not comparable with new ones. Raises
    SystemExit naming the --redo that makes the session consistent."""
    old = S.s["device"].get("fw")
    stale = sorted({t["step"] for t in S.s["plan"] if t.get("status") == "done"
                    and (t.get("fw") or old) not in (None, fw)}, key=STEPS.index)
    if S.s.get("step3_done") and old and old != fw and tab_def != S.defaults():
        stale = sorted(set(stale) | {"3"}, key=STEPS.index)
    if stale:
        raise SystemExit(
            f"firmware changed since these recordings: {old} -> {fw}. Recordings from two "
            f"firmwares do not compare: rerun with --redo {','.join(stale)} to record them again "
            "on this one, or flash the session's firmware back.")


async def resolve_gain(a, S, ui, dev, g, restore_gain):
    """Step 1's saved-gain decision. On a resume the session's earlier choice
    (gain_policy) is reused, never re-asked; a different choice, or a
    different effective gain, is refused while recordings made with the old
    one remain (--redo them). Returns the gain to restore at the end."""
    dv = S.s["device"]
    prev_eff = dv.get("gain_effective")
    prev_pol = dv.get("gain_policy")
    eff = None
    if g == st_gain(S):
        eff = g
        ui.info(f"saved mic gain {g}: start{{gain={st_gain(S)}}} is in effect")
    elif g == 0:
        # gain() reads 0 both when nothing is saved and when 0 is saved:
        # step 2a repeats one reply at start{gain=0} and compares levels
        if dv.get("gain_probe") is None:
            dv["gain_effective"] = f"{st_gain(S)} or 0"
            add_gain_probe(S)
            ui.info(f"gain() reads 0: no saved gain, or a saved 0. Step 2a checks which "
                    f"(one extra reply at start{{gain=0}}).")
    else:
        ui.warn(f"a saved mic gain {g} overrides start{{gain={st_gain(S)}}} (audio/gain setting)")
        rec = recorded_steps(S)
        if prev_pol and a.gain_policy_explicit and a.gain_policy != prev_pol and rec:
            raise SystemExit(
                f"this session recorded with --gain-policy {prev_pol}; --gain-policy {a.gain_policy} "
                f"would change the mic gain mid-session. Drop the option, or rerun with --redo "
                f"{','.join(rec)} to record everything again.")
        if prev_pol and not (a.gain_policy_explicit and a.gain_policy != prev_pol):
            pol = prev_pol
            ui.info(f"this session's gain choice: {pol}")
        else:
            pol = a.gain_policy if a.gain_policy != "ask" else (
                "set" if (await ui.ask(f"Set it to {st_gain(S)} for this session and restore {g} "
                                       "at the end? [Y/n]", "yn", "y")) == "y" else "keep")
        eff = st_gain(S) if pol == "set" else g
        if isinstance(prev_eff, int) and prev_eff != eff and recorded_steps(S):
            raise SystemExit(
                f"the session's recordings ran at mic gain {prev_eff}; this run would use {eff} "
                f"(saved gain() now reads {g}). Rerun with --redo {','.join(recorded_steps(S))} "
                "to record everything again.")
        dv["gain_policy"] = pol
        if pol == "set":
            if restore_gain is None:
                # recorded BEFORE the change, so a crash mid-way still restores it
                restore_gain = g
                dv["gain_restore"] = g
                S.save()
            await dev.set_gain(st_gain(S))
            ui.info(f"saved gain set to {st_gain(S)}; {restore_gain} is restored at the end")
        else:
            ui.warn(f"keeping gain {g}: captures run at gain {g}")
    if eff is not None:
        if isinstance(prev_eff, int) and prev_eff != eff and recorded_steps(S):
            raise SystemExit(
                f"the session's recordings ran at mic gain {prev_eff}; the device now runs {eff} "
                f"(saved gain() reads {g}). Rerun with --redo {','.join(recorded_steps(S))}, or "
                f"set it back: REPL {gain_cmd(prev_eff)}")
        dv["gain_effective"] = eff
    S.save()
    return restore_gain


async def session(a):
    # ---- session dir
    if a.resume:
        root = a.resume
        if root == "latest":
            ds = sorted(d for d in os.listdir(a.sessions) if os.path.exists(
                os.path.join(a.sessions, d, "session.json")))
            if not ds:
                raise SystemExit("no session to resume")
            root = os.path.join(a.sessions, ds[-1])
        S = C.Session(root)
        if not S.s:
            raise SystemExit(f"{root}: not a session")
        if a.name and a.name != S.s["settings"]["name"]:
            raise SystemExit(f"{root} is a session of {S.s['settings']['name']!r}, not {a.name!r}: "
                             "drop --name to resume it, or start a new session for that device")
        if "desk" in S.s.get("mode", ""):
            a.desk = True
            a.afplay_volume = S.s["settings"].get("afplay_volume") or a.afplay_volume
            if a.afplay_max is None:
                a.afplay_max = S.s["settings"].get("afplay_max")
        name = S.s["settings"]["name"]
        for r in (a.redo or "").split(","):
            if r:
                for t in S.trials(step=r):
                    t["status"] = "todo"
                if r in ("2a", "2b", "2c", "3"):
                    S.s["step3_done"] = False
        S.save()
    else:
        if not a.name and not a.sim:
            raise SystemExit("--name is required (or --resume)")
        name = a.name or "Sim Halo"
        slug = name.lower().replace(" ", "-") + ("-desk" if a.desk else "") + ("-sim" if a.sim else "")
        root = os.path.join(a.sessions, datetime.now().strftime("%Y%m%d-%H%M%S-") + slug)
        S = C.Session(root)
        S.s = dict(created=datetime.now().isoformat(timespec="seconds"),
                   mode=("sim " if a.sim else "") + ("desk" if a.desk else "worn"),
                   settings=dict(name=name, volume=a.volume, spk_gain=a.spk_gain, budget=a.budget,
                                 mic_gain=a.mic_gain, lead_s=LEAD_S, tail_s=TAIL_S,
                                 afplay_volume=a.afplay_volume if a.desk else None,
                                 afplay_max=a.afplay_max if a.desk else None,
                                 desk_voice_dbfs=C.DESK_VOICE_LEVEL_DBFS if a.desk else None),
                   device={}, plan=build_plan(a), step3_done=False)
        C.copy_clips(os.path.join(C.CLIPS_DIR, "replies"), os.path.join(root, "clips"))
        S.s["clips"] = json.load(open(os.path.join(root, "clips", "clips.json")))
        S.save()
    S.log(f"session start ({'resume' if a.resume else 'new'}), argv {sys.argv[1:]}")
    clips_blob = {c: open(os.path.join(root, "clips", f"{c}.lc3"), "rb").read() for c in S.s["clips"]}
    ui = UI(a)
    player = DeskPlayer(a) if (a.desk and not a.sim) else None
    if a.sim:
        from sim_device import SimDevice
        dev = SimDevice(a, S.log, Abort, LEAD_S, TAIL_S)
    else:
        dev = Ble(name, S.log)
    plan = S.s["plan"]
    todo = [t for t in plan if t["status"] != "done"]
    ui.header(f"Halo AEC calibration: {name}  ({S.s['mode']})",
              [f"session: {root}",
               f"{len(todo)} of {len(plan)} recordings to go, about "
               f"{est_minutes(todo, not S.s.get('step3_done')):.0f} minutes.",
               "Close the Noa app / disconnect the phone first (one BLE connection at a time).",
               "Ctrl+C stops safely; rerun with --resume latest to continue."])
    await ui.enter()
    ui.info(f"connecting to {name} ...")
    await dev.connect()
    restore_gain = S.s["device"].get("gain_restore")
    has_tune = False
    try:
        await dev.ready()
        # ---- step 1 facts (every run, so a resume notices a reflash)
        fw = await dev.fw()
        tab, tab_def = await dev.tune_tables()
        has_tune = tab is not None
        check_firmware(S, fw, tab_def)
        g = await dev.saved_gain()
        S.s["device"].update(fw=fw, aec_tune=tab, aec_defaults=tab_def, gain_saved=g)
        ui.info(f"firmware {fw}; aec_tune {'present (' + str(len(tab)) + ' keys)' if has_tune else 'absent: step 4 will be skipped'}")
        if has_tune and C.diff(tab, tab_def):
            ui.warn(f"the device runs a non-default aec_tune {C.diff(tab, tab_def)}; "
                    "the calibration compares against the defaults")
        restore_gain = await resolve_gain(a, S, ui, dev, g, restore_gain)
        tune_keys = None
        for step in STEPS:
            if step not in a.steps.split(","):
                continue
            if step == "3":
                if not S.s.get("step3_done"):
                    ui.header(STEP_TITLES[step], STEP_HELP[step])
                    try:
                        await step3(a, S, ui)
                    except Exception as e:
                        S.log(f"step 3 failed: {e!r}")
                        ui.warn(f"step 3 failed ({e!r}); step 4 will test B15")
                continue
            if step == "4":
                if not has_tune:
                    S.log("step 4 skipped: no frame.microphone.aec_tune")
                    continue
                tune_keys = rec_tune_keys(S)
                old = S.s.get("step4_tune")
                again = [t for t in plan if t["step"] == "4" and t["status"] == "done"]
                if old is not None and old != tune_keys and again:
                    # step 3 changed its pick since step 4 started: an A/B
                    # pools only recordings of one 'rec', so start step 4 over
                    S.log(f"step 4 set changed {old} -> {tune_keys}: recording step 4 again")
                    ui.warn(f"the set step 4 tests changed since its recordings ({old.get('rec')} -> "
                            f"{tune_keys['rec']}): step 4 starts over")
                    for t in again:
                        t["status"] = "todo"
                S.s["step4_tune"] = tune_keys
                S.save()
            ts = [t for t in plan if t["step"] == step and t["status"] != "done"]
            if not ts:
                continue
            ui.header(STEP_TITLES[step], STEP_HELP[step]
                      + ([f"Recommended set to test: {tune_keys['rec']}"] if step == "4" else []))
            await ui.enter()
            for i, t in enumerate(ts):
                while True:
                    ui.header(f"{STEP_TITLES[step]}  -  {i + 1}/{len(ts)}", trial_help(t))
                    pcm = await run_trial(a, S, dev, ui, t, player, clips_blob, tune_keys)
                    w = quick_check(S, ui, t, pcm)
                    if w:
                        ui.warn(w)
                        if t["prompts"] and (await ui.ask("[r]edo this one or [c]ontinue? [c]", "rc", "c")) == "r":
                            t["status"] = "todo"
                            S.save()
                            continue
                    break
                await ui.sleep(1.0)
        ui.header("Recording done", ["Restoring the device and writing the report ..."])
    finally:
        # session.json is the record (resolve_gain saves it before changing
        # the gain, so it is there even if that call is what failed)
        restore_gain = S.s["device"].get("gain_restore")
        failed = await finish_device(dev, has_tune, restore_gain, root)
        if restore_gain is not None and not any(w.startswith("saved mic gain") for w, _, _ in failed):
            S.s["device"]["gain_restored"] = True
            S.s["device"].pop("gain_restore", None)
            S.save()
        for what, err, fix in failed:
            S.log(f"finish: {what} failed: {err}")
        if failed:
            print("\n !! could not put the device back completely. Not restored:", flush=True)
            for what, err, fix in failed:
                print(f"    - {what} ({err})\n      fix: {fix}", flush=True)
    import calib_report
    rep = calib_report.write_report(S.root, silero=a.silero)
    ui.info(f"report: {rep}")
    return S


def st_gain(S):
    return S.s["settings"]["mic_gain"]


def add_gain_probe(S):
    """An extra 2a recording of reply A at start{gain=0}, right after the
    first gain-1 one: about 6 dB lower if start{gain=} takes effect (nothing
    saved), the same level if a saved gain (0) overrides it."""
    plan = S.s["plan"]
    if any(t["kind"] == "probe" for t in plan) or st_gain(S) == 0:
        return
    i = next((k for k, t in enumerate(plan) if t["step"] == "2a" and t["clip"] == "A"), None)
    if i is None:
        return
    plan.insert(i + 1, dict(plan[i], id="2a-gainprobe-A", kind="probe", mic_gain=0, status="todo"))
    S.save()


def gain_probe_verdict(S, ui, pcm):
    ref = [t for t in S.trials(step="2a", kind="echo", done=True) if t["clip"] == "A"]
    if not ref:
        return
    def lvl(x):
        e = C.fe(x)[int(1.2 * 50):-int(1.2 * 50)]
        return float(C.db(np.percentile(e, 75)))
    d = lvl(pcm) - lvl(S.load_trial(ref[0]["id"])["x"])
    g = st_gain(S)
    if d < -3:
        S.s["device"].update(gain_probe=round(d, 1), gain_effective=g)
        ui.info(f"start{{gain=0}} read {d:+.1f} dB vs gain {g}: no saved gain, gain {g} is in effect")
    elif d > -1.5:
        S.s["device"].update(gain_probe=round(d, 1), gain_effective=0)
        ui.warn(f"start{{gain=0}} read {d:+.1f} dB vs gain {g}: a saved gain 0 overrides start{{gain=}}; "
                "the captures run at gain 0 (6 dB low). frame.microphone.gain(1) would change it "
                "(it persists).")
    else:
        S.s["device"].update(gain_probe=round(d, 1))
        ui.warn(f"start{{gain=0}} read {d:+.1f} dB vs gain {g}: inconclusive")
    S.save()


def trial_help(t):
    if t["kind"] == "floor":
        return ["Stay silent for a few seconds."]
    if t["kind"] in ("echo", "probe"):
        return [f"Reply {t['clip']} plays" + (f" (AEC on, set '{t['tune']}')" if t["aec"] else "")
                + ". Stay silent and still."]
    if t["kind"] == "wearer":
        return [f"Read each phrase when it says NOW, in {LEVEL_TEXT[t['level']]}."]
    return [f"Reply {t['clip']} plays" + (f" (AEC on, set '{t['tune']}')" if t["aec"] else "")
            + ". Interrupt it when it says NOW:"] + [f"   at ~{p['at']:.1f} s: \"{p['text']}\""
                                                     for p in t["prompts"]]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", help="exact BLE name, e.g. 'Halo 28'")
    ap.add_argument("--desk", action="store_true",
                    help="desk dry run: the Mac speaker plays the wearer's phrases (afplay)")
    ap.add_argument("--afplay-volume", type=float, default=0.3,
                    help="--desk: afplay -v for the normal pass (quiet x0.5); default 0.3; "
                         "1.0 = a realistic voice level at the mic (the desk_aec talker's)")
    ap.add_argument("--afplay-max", type=float, default=None,
                    help="--desk: cap for the loud pass (x2); default = --afplay-volume, so "
                         "nothing plays louder than the normal pass unless you raise it")
    ap.add_argument("--resume", help="session dir, or 'latest'")
    ap.add_argument("--redo", help="with --resume: steps to record again, e.g. '2a,4'")
    ap.add_argument("--steps", default=",".join(STEPS), help="steps to run (default all)")
    ap.add_argument("--sessions", default=os.path.join(HERE, "sessions"))
    ap.add_argument("--echo-reps", type=int, default=2)
    ap.add_argument("--confirm-reps", type=int, default=1, choices=(1, 2),
                    help="step 4 clip passes (1: clip B, 2: B and C)")
    ap.add_argument("--search-budget", type=float, default=90, help="step 3 search time (s)")
    ap.add_argument("--retries", type=int, default=4, help="BLE retries per recording")
    ap.add_argument("--volume", type=int, default=100)
    ap.add_argument("--spk-gain", type=int, default=6)
    ap.add_argument("--budget", type=int, default=100)
    ap.add_argument("--mic-gain", type=int, default=1)
    ap.add_argument("--gain-policy", choices=("ask", "keep", "set"), default=None,
                    help="when a saved gain() overrides start{gain=}: ask (worn default), keep, "
                         "or set (restored at the end)")
    ap.add_argument("--guard", nargs="+", metavar="SESSION",
                    help="step 3: sittings the search must not lose near end on (default: every "
                         "other non-sim session in --sessions)")
    ap.add_argument("--no-guard", action="store_true", help="step 3: no guard sittings")
    ap.add_argument("--silero", action="store_true", help="report: Silero VAD on the captures (torch)")
    ap.add_argument("--sim", action="store_true", help="simulated device (no BLE)")
    ap.add_argument("--fast", action="store_true", help="--sim: no waits")
    ap.add_argument("--sim-drop", type=float, default=0.0, help="--sim: drop probability per BLE step")
    ap.add_argument("--sim-abort-after", type=int, default=0, help="--sim: die after N recordings")
    ap.add_argument("--sim-no-tune", action="store_true", help="--sim: firmware without aec_tune")
    ap.add_argument("--sim-old-gate", action="store_true",
                    help="--sim: aec_tune without the gate band and two-band cap keys")
    ap.add_argument("--sim-saved-gain", type=int, default=0)
    ap.add_argument("--sim-fw", help="--sim: firmware version string (default sim-0.8.18)")
    ap.add_argument("--sim-fail", help="--sim: device calls that fail: set_gain,finish_tune")
    ap.add_argument("--sim-coupling-db", type=float, default=-9.0,
                    help="--sim: echo path gain (-9 ~ -30 dBFS echo, desk Halo 28 mid; -3 ~ worn)")
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    a.gain_policy_explicit = a.gain_policy is not None
    if a.gain_policy is None:
        a.gain_policy = "ask" if not (a.desk or a.sim) else "keep"
    if a.fast and not a.sim:
        ap.error("--fast is for --sim only")
    os.makedirs(a.sessions, exist_ok=True)
    C.ensure_clips()
    if not a.sim and shutil.which("caffeinate"):
        # macOS: the display sleeps after 10 min and its speakers go silent
        # (desk mode), and the Mac must not sleep mid-sitting: keep it awake
        subprocess.Popen(["caffeinate", "-dui", "-w", str(os.getpid())])
    try:
        asyncio.run(session(a))
    except (Abort, KeyboardInterrupt) as e:
        print(f"\n !! stopped: {e!r}\n    Everything recorded so far is saved. Continue with:\n"
              f"    uv run halo_calib.py --resume latest" + (" --sim" if a.sim else ""))
        sys.exit(2)


if __name__ == "__main__":
    main()
