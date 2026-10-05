"""Simulated Halo for dry runs of halo_calib.py without BLE (--sim).

Same interface as halo_calib.Ble. The 'acoustics' are synthetic but go
through the real pieces where it matters:
  echo   = the uploaded LC3 clip -> speaker_protect model (spk_ref)
           -> short decaying IR x coupling -> soft clip (transducer)
  wearer = the --desk `say` voice for each prompt (0.35 s reaction), with a
           low-band boost (bone conduction), at -24 dBFS (quiet -6, loud +6 dB)
  mic    = noise floor (-58 dBFS) + echo + wearer, voice band-pass, and the
           tree's audio_aec.c (aec_replay with the current aec_tune table) when
           aec=true, then LC3.
diag rows carry capn, p_mic (50 ms raw mic power) and p_ref every 60 ms.
--sim-drop P raises a BLE-like error on a random step with probability P;
--sim-abort-after N kills the session after N recordings (resume test).
--sim-fw overrides the firmware string (a reflash between runs); --sim-fail
makes device calls fail: set_gain (every call), finish_tune (aec_tune
('defaults') at the end).
"""
import asyncio
import os
import tempfile
import time

import numpy as np

import calib_common as C
import calib_offline as O

ITEM6_KEYS = ["gate_band_hz", "cap_split_hz", "cap_lo_gcap"]


class SimDrop(RuntimeError):
    pass


class SimDevice:
    def __init__(self, a, log, abort_cls, lead_s, tail_s):
        self.Abort, self.LEAD, self.TAIL = abort_cls, lead_s, tail_s
        self.a, self.log = a, log
        self.rng = np.random.default_rng(a.seed)
        self.has_tune = not a.sim_no_tune
        self.saved_gain_v = a.sim_saved_gain
        self.clips, self.refs = {}, {}
        self.clip_loaded = None
        self.n_done = 0
        self.voice = C.ensure_desk_voice()
        self.defaults = {}
        if self.has_tune:
            # the tree's real defaults; --sim-old-gate: firmware before the gate band
            self.defaults = dict(O.tree_defaults())
            if a.sim_old_gate:
                for k in ITEM6_KEYS:
                    self.defaults.pop(k, None)
                self.defaults.update(C.NAMED["old-gate"])
                for k in ITEM6_KEYS:
                    self.defaults.pop(k, None)
        self.tune = dict(self.defaults)
        self.coupling = 10 ** (a.sim_coupling_db / 20)
        # fixed 'head': 6 ms decaying IR, coupling -> ~-27 dBFS echo
        n = 96
        self.ir = self.rng.standard_normal(n) * np.exp(-np.arange(n) / 20.0)
        self.ir[0] += 2.0
        self.ir *= 1.2 / np.sqrt((self.ir ** 2).sum())

    def _maybe_drop(self, where):
        if self.a.sim_drop and self.rng.random() < self.a.sim_drop:
            raise SimDrop(f"sim BLE drop (reason 0x98) during {where}")

    async def connect(self):
        self.clip_loaded = None

    async def disconnect(self):
        pass

    async def ready(self):
        pass

    async def reconnect(self):
        self.clip_loaded = None

    async def fw(self):
        if self.a.sim_fw:
            return self.a.sim_fw
        return "sim-0.8.18" if self.has_tune else "sim-0.8.17"

    def _fail(self, what):
        if what in (self.a.sim_fail or "").split(","):
            raise RuntimeError(f"sim: {what} failed")

    async def tune_table(self):
        return dict(self.tune) if self.has_tune else None

    async def tune_tables(self, defaults=True):
        if not self.has_tune:
            return None, None
        return dict(self.tune), (dict(self.defaults) if defaults else None)

    async def tune_apply(self, keys):
        self._maybe_drop("aec_tune")
        bad = [k for k in keys if k not in self.defaults]
        if bad:
            raise SystemExit(f"aec_tune rejected the set: unknown key {bad[0]}")
        self.tune = dict(self.defaults)
        for k, v in keys.items():
            self.tune[k] = (int(v) // 20) * 20 if k.endswith("_ms") else v
        return dict(self.tune)

    async def saved_gain(self):
        return self.saved_gain_v

    async def set_gain(self, g):
        self._fail("set_gain")
        self.saved_gain_v = int(g)

    async def upload(self, cid, blob):
        if self.clip_loaded == cid:
            return
        self._maybe_drop("upload")
        self.clips[cid] = C.lc3_decode(blob)
        self.clip_loaded = cid

    def _ref(self, cid, st):
        if cid not in self.refs:
            with tempfile.TemporaryDirectory() as td:
                self.refs[cid] = O.spk_ref(self.clips[cid], st["volume"], st["spk_gain"],
                                           st["budget"], td, cid)
        return self.refs[cid]

    def _aec(self, mic, ref_placed, feed):
        # old-gate firmware: the missing keys at their old values (off)
        table = C.apply(O.tree_defaults(), dict(C.NAMED["old-gate"], **self.tune)
                        if self.a.sim_old_gate else self.tune)
        with tempfile.TemporaryDirectory() as td:
            C.write_wav(os.path.join(td, "m.wav"), mic)
            C.write_wav(os.path.join(td, "r.wav"), ref_placed)
            out = O.run(os.path.join(td, "m.wav"), os.path.join(td, "r.wav"), table, feed=feed)["out"]
            # O.run re-aligns; the device output carries the AEC delay
            return C.shift(out, -C.AEC_DELAY)

    async def capture(self, st, aec, tts, seconds, on_start, prompts=()):
        self._maybe_drop("capture start")
        lead, tail = self.LEAD, self.TAIL
        n = int((lead + seconds + tail) * C.SR) // C.HOP * C.HOP
        t_play = lead + 0.02
        noise = C.bandpass(self.rng.standard_normal(n), 200, 7000)
        x = noise * 10 ** (-58 / 20) / (np.std(C.bandpass(noise, 300, 3400)) + 1e-12)
        ref_placed = np.zeros(n)
        feed = (0, 0)
        if tts:
            ref = self._ref(self.clip_loaded, st)
            s = int(t_play * C.SR)
            m = min(len(ref), n - s)
            ref_placed[s:s + m] = ref[:m]
            echo = np.convolve(ref_placed, self.ir)[:n] * self.coupling
            echo = 0.3 * np.tanh(echo / 0.3)
            x = x + echo
            feed = (s // C.HOP * C.HOP, min(n, (s + m + C.HOP - 1) // C.HOP * C.HOP))
        await on_start(time.monotonic())
        for p in prompts:
            v = C.read_wav(self.voice[p["text"]])
            v = v + 1.5 * C.bandpass(v, 80, 700)          # bone-conduction low-band boost
            v *= 10 ** ((-24 - C.active_rms_db(v)) / 20)
            v *= {"normal": 1.0, "quiet": 0.5, "loud": 2.0}[p.get("level") or "normal"]
            s = int((t_play + p["at"] + 0.35) * C.SR)
            m = max(0, min(len(v), n - s))
            x[s:s + m] += v[:m]
        self._maybe_drop("readback")
        # PDM gain: a saved gain overrides start{gain=} (sim: 0 = nothing saved)
        g = self.saved_gain_v if self.saved_gain_v else st["mic_gain"]
        x = x * 10 ** ((g - 1) * 6.0 / 20)
        mic = C.bandpass(x, 300, 3400)
        out = self._aec(mic, np.roll(ref_placed, -24), feed) if aec else mic
        raw = C.lc3_encode(out)
        pcm = C.lc3_decode(raw)
        diag = []
        a = 1.0 / (0.05 * C.SR)
        pm = pr = 0.0
        hp = C.bandpass(x, 80, 8000)
        k = 0
        start = int(lead * C.SR)
        for i in range(start, int((lead + seconds) * C.SR), C.HOP):
            blk = hp[i:i + C.HOP]
            rb = ref_placed[i:i + C.HOP]
            pm = pm + (1 - (1 - a) ** C.HOP) * (np.mean(blk ** 2) - pm)
            pr = pr + (1 - (1 - a) ** C.HOP) * (np.mean(rb ** 2) - pr)
            if k % 3 == 0:
                diag.append([i / C.SR * C.LC3_BPS, 0, 0, pm, pr, 0, 1.0, 0, 0, 0, 0, 1, 0])
            k += 1
        self.n_done += 1
        if self.a.sim_abort_after and self.n_done >= self.a.sim_abort_after:
            self.a.sim_abort_after = 0
            raise self.Abort("sim: session killed (resume test)")
        await asyncio.sleep(0)
        return pcm, raw, diag, dict(target_bytes=len(raw), got_bytes=len(raw), sim=True)

    async def tune_defaults(self):
        self._fail("finish_tune")
        self.tune = dict(self.defaults)

    async def remove_capture(self):
        pass

    async def reset(self):
        pass
