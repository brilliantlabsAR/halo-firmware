#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10,<3.13"
# dependencies = [
#     "brilliant-msg",
#     "lc3py",
#     "numpy",
#     "websockets",
# ]
# ///
"""BLE-fed duplex barge-in test: Halo <-> a local OpenAI-Realtime server.

The end-to-end test the bench harnesses can't do: mic LC3 and speaker LC3
both stream over BLE while a real server-side VAD (s2s: huggingface/speech-to-speech,
Silero v5) decides when the wearer "spoke". Full duplex by default - the mic
is NEVER muted, so any AEC residual the VAD confirms fires a false barge-in,
exactly like production.

What it records, for offline blame assignment:
  - duplex_mic_<ts>.wav      - the decoded mic stream the server heard
                               (AEC'd, through LC3+BLE). In duplex mode the
                               server's audio clock equals this wav's
                               timeline, so its speech_started
                               audio_start_ms indexes straight into the wav.
                               Run vad_gauntlet.py on it to re-judge offline.
  - duplex_events_<ts>.jsonl - timestamped events: server VAD
                               speech_started/stopped (+audio_start_ms),
                               response lifecycle, playback spans (first
                               frame sent / queue drained, with the mic-clock
                               position of each), device aec('stats') dump.

After Ctrl-C the frame app is stopped and aec('stats') is read over the
REPL: feed_gaps/feed_gap_ms and resyncs are the BLE-starvation ground truth
(nonzero => the speaker feed starved and the canceller resynced; fix pacing
before blaming the filter or the VAD).

Usage (s2s on the LAN, no key):
    uv run duplex_vad_test.py --name "Halo AB"
    uv run duplex_vad_test.py --half-duplex          # control run
    uv run duplex_vad_test.py --threshold 0.7 --silence-ms 200

Requires the SDK checkout for the Lua frame app (--lua to override).
"""

import argparse
import asyncio
import base64
import json
import os
import signal
import time
import wave
from datetime import datetime

import lc3
import numpy as np
from websockets.asyncio.client import connect

from brilliant_ble import BrilliantDeviceType
from brilliant_msg import BrilliantMsg, RxAudio, TxCode

# message codes, must match lua/duplex_frame_app.lua (an AEC-instrumented
# copy of the SDK's openai_realtime_frame_app.lua that adds STATS_MSG)
CLEAR_MSG = 0x10
TEXT_MSG = 0x12
START_LISTENING_MSG = 0x30
STOP_LISTENING_MSG = 0x31
STATS_MSG = 0x32
START_PLAYBACK_MSG = 0x40
STOP_PLAYBACK_MSG = 0x41


def mic_start_payload(gain: int, aec: bool, voice: bool) -> bytes:
    """START_LISTENING payload: three named bytes, one field each -- no
    cryptic offset packing. Decoded by name in the frame app (see
    lua/duplex_frame_app.lua START_LISTENING handler):
        [0] gain code 0..20 (10 = 0 dB)   [1] aec 0/1   [2] voice 0/1
    This maps 1:1 onto frame.microphone.start{gain=, aec=, voice=}; copy this
    shape (e.g. a TxMicStart message) into SDK samples rather than the old
    gain+100/+200 magnitude trick.
    """
    return bytes([max(0, min(20, gain)), 1 if aec else 0, 1 if voice else 0])

SR = 16000                       # Halo LC3 coded rate (mic + speaker)
WIRE_RATE = 24000                # the GA AudioPCM model pins rate to 24000
FRAME_US = 10000
BITRATE = 32000
LC3_FRAME_BYTES = BITRATE // 8 * FRAME_US // 1_000_000   # 40
PCM_FRAME_BYTES = WIRE_RATE // 100 * 2                   # 10 ms wire PCM16

# Local s2s (huggingface/speech-to-speech) realtime VAD server. Default to
# localhost; when it runs on another box on the LAN pass --url with that host's
# IP (the address is dynamic, so it isn't hard-coded here).
DEFAULT_URL = "ws://localhost:8765/v1/realtime"
DEFAULT_LUA = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "lua", "duplex_frame_app.lua")


class EventLog:
    def __init__(self, path):
        self.f = open(path, "w")
        self.t0 = time.monotonic()

    def log(self, etype, mic_ms=None, **kw):
        rec = {"t": round(time.monotonic() - self.t0, 3), "event": etype}
        if mic_ms is not None:
            rec["mic_ms"] = int(mic_ms)
        rec.update(kw)
        self.f.write(json.dumps(rec) + "\n")
        self.f.flush()
        extra = " ".join(f"{k}={v}" for k, v in rec.items()
                         if k not in ("t", "event"))
        print(f"[{rec['t']:8.2f}] {etype} {extra}")


class Session:
    def __init__(self, frame, ws, frames_per_write, events, duplex,
                 pace_factor, onset_guard_ms=0,
                 barge_confirm_frames=2, barge_rms=150.0):
        self.frame = frame
        self.ws = ws
        self.frames_per_write = frames_per_write
        self.events = events
        self.duplex = duplex
        self.pace_factor = pace_factor
        # onset guard: when >0, the SERVER's auto-interrupt is disabled and
        # the client cancels a reply on speech_started ONLY after it has been
        # playing this long. Suppresses the cold-start onset residual that
        # would otherwise kill every reply in its first word before the AEC
        # converges (the head-worn barge-in cascade). 0 => server-side interrupt.
        self.onset_guard_ms = onset_guard_ms
        self.reply_onset_t = None    # monotonic time of this reply's first audio
        self.barged = False          # client has cancelled the current reply
        # Barge-in confirmation (default server-interrupt mode): a server
        # speech_started only ARMS the client flush; pump_mic then requires
        # barge_confirm_frames consecutive mic reads whose (post-AEC) RMS clears
        # barge_rms before it actually drains the speaker_queue. A single
        # spurious VAD trip (a residual-echo blip) never confirms, so it can't
        # cut off a genuine reply while the wearer is silent. The client mic
        # RMS is the immediate near-end signal (unlike speech_stopped, which
        # lags by the server VAD's silence hangover).
        self.barge_confirm_frames = barge_confirm_frames
        self.barge_rms = barge_rms
        self.barge_armed = False     # a speech_started awaits mic confirmation
        self.barge_hot = 0           # consecutive above-threshold mic reads

        # liblc3 resamples inside the codec: coded 16 kHz <-> 24 kHz wire.
        # A second decoder yields a native-16k copy of the same mic frames
        # for the recording (what vad_gauntlet.py consumes).
        self.decoder = lc3.Decoder(FRAME_US, SR, 1,
                                   output_sample_rate_hz=WIRE_RATE)
        self.decoder_rec = lc3.Decoder(FRAME_US, SR, 1)
        self.encoder = lc3.Encoder(FRAME_US, SR, 1,
                                   input_sample_rate_hz=WIRE_RATE)
        # LC3-roundtripped copy of what the speaker was fed, stamped onto
        # the mic clock at send time (silence-filled between spans), so
        # episodes in the mic wav can be classified echo-vs-wearer by
        # coherence against this reference.
        self.decoder_ref = lc3.Decoder(FRAME_US, SR, 1)
        self.ref_pcm = bytearray()
        self.speaker_queue: asyncio.Queue = asyncio.Queue()
        self._mic_lc3_buf = bytearray()
        self._spk_pcm_buf = bytearray()
        self.response_active = False
        self.playback_open = False   # a playback span is in progress
        self.mic_pcm = bytearray()   # everything decoded from the mic
        self.mic_sent_samples = 0    # samples actually sent to the server

    def mic_ms(self):
        return len(self.mic_pcm) / 2 / SR * 1000

    def _speaking(self):
        return self.response_active or not self.speaker_queue.empty()

    async def pump_mic(self, mic_queue):
        while True:
            chunk = await mic_queue.get()
            if chunk is None:
                continue
            self._mic_lc3_buf += chunk
            pcm = bytearray()
            while len(self._mic_lc3_buf) >= LC3_FRAME_BYTES:
                fb = bytes(self._mic_lc3_buf[:LC3_FRAME_BYTES])
                del self._mic_lc3_buf[:LC3_FRAME_BYTES]
                pcm += self.decoder.decode(fb, bit_depth=16)
                self.mic_pcm += self.decoder_rec.decode(fb, bit_depth=16)
            if not pcm:
                continue
            # Barge-in confirmation: once a server speech_started has ARMED a
            # flush, require barge_confirm_frames consecutive mic reads whose
            # RMS clears barge_rms before draining the speaker_queue. This is
            # the "a couple of readings above threshold" debounce - a single
            # spurious VAD trip that isn't backed by sustained near-end energy
            # never reaches the count and leaves the reply playing.
            if self.barge_armed and not self.barged:
                samples = np.frombuffer(bytes(pcm), dtype=np.int16)
                # DC-removed (AC) RMS: the device mic carries a large DC bias
                # (~1265 in the captures) that would swamp a raw RMS and defeat
                # the threshold, so measure the AC energy (std) instead.
                rms = float(np.std(samples.astype(np.float64))) \
                    if samples.size > 1 else 0.0
                if rms >= self.barge_rms:
                    self.barge_hot += 1
                    if self.barge_hot >= self.barge_confirm_frames:
                        age_ms = (1e3 * (time.monotonic() - self.reply_onset_t)
                                  if self.reply_onset_t is not None else 0.0)
                        self.barge_armed = False
                        self.barge_hot = 0
                        await self._client_barge_in(age_ms)
                else:
                    self.barge_hot = 0
            if not self.duplex and self._speaking():
                continue  # half-duplex control mode: mute while speaking
            self.mic_sent_samples += len(pcm) // 2
            await self.ws.send(json.dumps({
                "type": "input_audio_buffer.append",
                "audio": base64.b64encode(bytes(pcm)).decode("ascii"),
            }))

    def _enqueue_playback(self, pcm):
        self._spk_pcm_buf += pcm
        while len(self._spk_pcm_buf) >= PCM_FRAME_BYTES:
            fp = bytes(self._spk_pcm_buf[:PCM_FRAME_BYTES])
            del self._spk_pcm_buf[:PCM_FRAME_BYTES]
            self.speaker_queue.put_nowait(
                self.encoder.encode(fp, LC3_FRAME_BYTES, bit_depth=16))

    async def pump_speaker(self):
        while True:
            frames = [await self.speaker_queue.get()]
            if not self.playback_open:
                self.playback_open = True
                if self.reply_onset_t is None:
                    self.reply_onset_t = time.monotonic()
                self.events.log("playback_start", mic_ms=self.mic_ms())
            while len(frames) < self.frames_per_write:
                try:
                    frames.append(self.speaker_queue.get_nowait())
                except asyncio.QueueEmpty:
                    break
            await self.frame.send_audio(b"".join(frames),
                                        await_bt_response=False)
            pad = int(self.mic_ms() / 1000 * SR) * 2 - len(self.ref_pcm)
            if pad > 0:
                self.ref_pcm += bytes(pad)
            for fr in frames:
                self.ref_pcm += self.decoder_ref.decode(fr, bit_depth=16)
            await asyncio.sleep(len(frames) * 0.01 * self.pace_factor)
            if self.speaker_queue.empty() and not self.response_active \
                    and self.playback_open:
                self.playback_open = False
                # close the reply clock so the next span re-stamps it: s2s
                # never sends response.created here to reset it, which otherwise
                # made client_barge_in's age_ms grow unbounded across the run.
                self.reply_onset_t = None
                self.events.log("playback_drained", mic_ms=self.mic_ms())

    async def auto_prompt(self, text, interval):
        """Drive replies without a human: send a text turn whenever the
        assistant is idle. For unattended on-desk validation - the echo
        dynamics (playback -> residual -> LC3 -> server VAD) are
        identical, and with nobody in the room EVERY confirmed
        speech_started is an AEC-residual false barge-in, which makes
        the pass criterion sharper than a worn run.
        """
        n = 0
        while True:
            await asyncio.sleep(3.0 if n == 0 else interval)
            if self._speaking():
                continue
            n += 1
            await self.ws.send(json.dumps({
                "type": "conversation.item.create",
                "item": {"type": "message", "role": "user",
                         "content": [{"type": "input_text", "text": text}]},
            }))
            await self.ws.send(json.dumps({"type": "response.create"}))
            self.events.log("auto_prompt", mic_ms=self.mic_ms(), n=n)

    async def _client_barge_in(self, age_ms):
        """Cancel the current reply from the client side: flush what hasn't
        emitted (this is what actually stops the echo, regardless of whether
        the server honours response.cancel) and drop the rest of its deltas."""
        # Only latch `barged` (drop the tail) if a reply is STILL generating -
        # then its trailing deltas must be dropped until response.done. If the
        # reply already completed (the common case: the server finishes ~1.4s
        # in and the barge lands seconds later), there is no tail to drop and
        # latching would wrongly mute the NEXT reply. Flushing speaker_queue
        # below is what stops the buffered voice either way.
        self.barged = self.response_active
        self.barge_armed = False
        self.barge_hot = 0
        self.reply_onset_t = None    # this span is over; next reply re-stamps
        dropped = 0
        while not self.speaker_queue.empty():
            self.speaker_queue.get_nowait()
            dropped += 1
        self._spk_pcm_buf.clear()
        try:
            await self.ws.send(json.dumps({"type": "response.cancel"}))
        except Exception:
            pass
        self.events.log("client_barge_in", mic_ms=self.mic_ms(),
                        age_ms=round(age_ms), frames=dropped)

    async def receive(self):
        async for message in self.ws:
            event = json.loads(message)
            etype = event.get("type")
            if etype == "response.created":
                self.response_active = True
                self.reply_onset_t = None
                self.barged = False
                self.events.log("response_created", mic_ms=self.mic_ms())
            elif etype == "response.output_audio.delta":
                if self.barged:
                    continue   # client barge-in: drop the rest of this reply
                # Track response_active off the audio stream itself, not just
                # response.created: s2s in server-VAD mode does not emit
                # response.created for auto-created replies, so keying anything
                # on it (response_active, the barged reset) silently breaks.
                # A delta means a reply is live; response.done ends it.
                self.response_active = True
                self._enqueue_playback(base64.b64decode(event["delta"]))
            elif etype == "response.done":
                self.response_active = False
                # A barge whose cancelled reply has now ended (or a reply that
                # completed before the barge) must stop dropping deltas, so the
                # NEXT reply plays. Without this the barged latch never cleared
                # in server-VAD mode (no response.created) and all audio after
                # the first barge went silent.
                self.barged = False
                status = (event.get("response") or {}).get("status")
                self.events.log("response_done", mic_ms=self.mic_ms(),
                                status=status)
                if status == "cancelled":
                    # barge-in: drop un-sent playback so the wav's playback
                    # spans reflect what actually kept playing
                    dropped = 0
                    while not self.speaker_queue.empty():
                        self.speaker_queue.get_nowait()
                        dropped += 1
                    self._spk_pcm_buf.clear()
                    if dropped:
                        self.events.log("playback_flushed",
                                        mic_ms=self.mic_ms(), frames=dropped)
            elif etype == "input_audio_buffer.speech_started":
                self.events.log("speech_started", mic_ms=self.mic_ms(),
                                audio_start_ms=event.get("audio_start_ms"))
                age_ms = (1e3 * (time.monotonic() - self.reply_onset_t)
                          if self.reply_onset_t is not None else 0.0)
                if self.onset_guard_ms:
                    if self.response_active and not self.barged:
                        if age_ms >= self.onset_guard_ms:
                            await self._client_barge_in(age_ms)
                        else:
                            self.events.log("barge_guarded",
                                            mic_ms=self.mic_ms(),
                                            age_ms=round(age_ms))
                elif not self.barged and (self.playback_open
                                          or not self.speaker_queue.empty()
                                          or self._spk_pcm_buf):
                    # Default (server-interrupt) mode: ARM a client-side flush.
                    # The realtime server finishes generating the whole reply
                    # ~1.4s into playback and closes it (response.done:
                    # completed), after which its VAD auto-interrupt has nothing
                    # left to cancel - but the client is still dribbling up to
                    # ~8s of queued audio out over BLE at real-time pace, so the
                    # assistant's voice keeps playing for seconds after the
                    # wearer barges. Draining speaker_queue is the only thing
                    # that stops that backlog. We don't drain on this single VAD
                    # reading, though: pump_mic requires a couple of consecutive
                    # above-threshold mic reads to CONFIRM first, so a lone
                    # spurious speech_started can't cut off a reply. See
                    # tests/aec/README.md "client-side flush gap" (2026-07-14).
                    self.barge_armed = True
                    self.barge_hot = 0
            elif etype == "input_audio_buffer.speech_stopped":
                self.events.log("speech_stopped", mic_ms=self.mic_ms(),
                                audio_end_ms=event.get("audio_end_ms"))
                # near-end ended before the client mic confirmed the barge:
                # drop the pending flush so it can't fire on a later hot read.
                self.barge_armed = False
                self.barge_hot = 0
            elif etype == "response.output_audio_transcript.delta":
                print(event.get("delta", ""), end="", flush=True)
            elif etype == "error":
                self.events.log("server_error", detail=event.get("error"))


def session_update(args):
    # onset guard puts barge-in under client control, so the server must not
    # auto-interrupt (it fires before the guard window can protect the reply).
    server_interrupt = (not args.no_interrupt) and not args.onset_guard_ms
    td = {"type": "server_vad", "interrupt_response": server_interrupt}
    if args.threshold is not None:
        td["threshold"] = args.threshold
    if args.silence_ms is not None:
        td["silence_duration_ms"] = args.silence_ms
    return {
        "type": "session.update",
        "session": {
            "type": "realtime",
            "output_modalities": ["audio"],
            "instructions": args.instructions,
            "audio": {
                "input": {
                    "format": {"type": "audio/pcm", "rate": WIRE_RATE},
                    "turn_detection": td,
                },
                "output": {"format": {"type": "audio/pcm",
                                      "rate": WIRE_RATE}},
            },
        },
    }


async def dump_aec_stats(frame, events):
    """Post-break cumulative dump (fallback; the mid-session sampler is
    the primary record). Printed in short batches: a single print of the
    whole table overflows the print buffer and drops arbitrary keys -
    the 2026-07-13 session dump lost pdm_dropped to exactly this.
    """
    printed = []
    frame.detach_print_response_handler()
    frame.attach_print_response_handler(lambda s: printed.append(s))
    await frame.ble.send_break_signal()
    await frame.ble.send_lua(
        "local s=frame.microphone.diag('stats') local o='' "
        "for k,v in pairs(s) do o=o..k..'='..tostring(v)..' ' "
        "if #o>150 then print(o) o='' end end "
        "if #o>0 then print(o) end print('#end')",
        await_print=True, timeout=10)
    for _ in range(100):
        if any(p.strip() == "#end" for p in printed):
            break
        await asyncio.sleep(0.05)
    stats = {}
    for line in printed:
        for kv in line.split():
            if "=" in kv:
                k, v = kv.split("=", 1)
                stats[k] = v
    events.log("aec_stats", **stats)
    return stats


class StatsSampler:
    """Samples aec('stats') MID-SESSION via the frame app's STATS_MSG
    (no REPL break, the app keeps running): margin(t), the PDM loss
    ledger, and the loss-accounting counters are only meaningful as a
    time series - the post-hoc cumulative read is the known trap.
    """

    KEY_FIELDS = ("fd_wnorm2", "sup_gmean", "sup_gate_rel", "sup_gate_fast",
                  "sup_gate_mid", "sup_onset",
                  "p_ref", "margin_last", "ref_skew_adj",
                  "ref_underruns", "resyncs", "ref_pads")

    def __init__(self, frame, events):
        self.frame = frame
        self.events = events
        self.cur = {}
        self.done = asyncio.Event()

    def on_print(self, s):
        if not s.startswith("#stats"):
            print(f"Frame: {s}")
            return
        body = s[len("#stats"):].strip()
        if body == "end":
            if self.cur:
                self.events.log("aec_stats_mid", **self.cur)
                brief = " ".join(f"{k}={self.cur[k]}"
                                 for k in self.KEY_FIELDS if k in self.cur)
                print(f"[stats] {brief}")
            self.cur = {}
            self.done.set()
        else:
            for kv in body.split():
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    self.cur[k] = v

    async def sample(self):
        self.done.clear()
        await self.frame.send_message(STATS_MSG, TxCode(1).pack())
        try:
            await asyncio.wait_for(self.done.wait(), 5)
        except asyncio.TimeoutError:
            print("[stats] sample timed out")

    async def run(self, interval):
        while True:
            await asyncio.sleep(interval)
            await self.sample()


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--url", default=DEFAULT_URL)
    p.add_argument("--name", default=None, help="BLE device name filter")
    p.add_argument("--lua", default=DEFAULT_LUA)
    p.add_argument("--half-duplex", action="store_true",
                   help="control mode: mute the mic while a reply plays")
    p.add_argument("--no-interrupt", action="store_true",
                   help="VAD still reported, but replies are not cancelled")
    p.add_argument("--onset-guard-ms", type=int, default=0,
                   help="client-side barge-in: ignore VAD for the first N ms "
                        "of each reply (breaks the cold-start onset cascade); "
                        "disables the server's auto-interrupt")
    p.add_argument("--barge-confirm-frames", type=int, default=2,
                   help="default (server-interrupt) mode: consecutive mic "
                        "reads whose RMS clears --barge-rms required to confirm "
                        "a speech_started before draining the speaker queue "
                        "(a couple of readings, so one spurious VAD trip can't "
                        "cut off a reply). 1 => flush on the first read")
    p.add_argument("--barge-rms", type=float, default=150.0,
                   help="AC (DC-removed) RMS a mic read must clear to count "
                        "toward --barge-confirm-frames (post-AEC near-end "
                        "energy; quiet mid-reply floor ~40-70, near-end ~600)")
    p.add_argument("--threshold", type=float, default=None,
                   help="server VAD threshold override (s2s default 0.6)")
    p.add_argument("--silence-ms", type=int, default=None)
    p.add_argument("--volume", type=int, default=80,
                   help="speaker volume 1..100 (AEC was validated at 80)")
    p.add_argument("--gain", type=int, default=10,
                   help="mic gain code 0..20 (10 = 0 dB)")
    p.add_argument("--aec-off", action="store_true",
                   help="disable on-device AEC (control baseline: raw echo, "
                        "same volume, to separate the filter from acoustic "
                        "bleed)")
    p.add_argument("--no-voice", dest="voice", action="store_false",
                   default=True,
                   help="disable voice mode (the [300,3800]Hz band-pass on the "
                        "AEC output that removes the out-of-band echo the server "
                        "VAD trips on); default on. Off = raw full-band AEC feed")
    p.add_argument("--seconds", type=float, default=None,
                   help="auto-stop after N seconds via the clean-shutdown path "
                        "(same as Ctrl-C: stops the app, dumps aec('stats'), "
                        "saves the wav). Unattended captures, e.g. the "
                        "startup-onset check.")
    p.add_argument("--stats-interval", type=float, default=10.0,
                   help="seconds between mid-session aec('stats') samples")
    p.add_argument("--auto-prompt", default=None,
                   help="unattended mode: text turn sent whenever the "
                        "assistant is idle, so replies keep coming with "
                        "nobody in the room (every confirmed speech_started "
                        "is then an AEC-residual false barge-in)")
    p.add_argument("--auto-interval", type=float, default=12.0,
                   help="idle seconds between auto-prompt turns")
    p.add_argument("--pace-factor", type=float, default=0.9,
                   help="speaker feed pacing vs realtime (lower = deeper "
                        "device queue, more starvation margin)")
    p.add_argument("--instructions",
                   default="You are a helpful assistant speaking to the "
                           "user through smart glasses. Answer in one or "
                           "two sentences.")
    return p.parse_args()


async def main():
    args = parse_args()
    ts = datetime.now().strftime("%H%M%S")
    wav_path = f"duplex_mic_{ts}.wav"
    events = EventLog(f"duplex_events_{ts}.jsonl")
    events.log("config", duplex=not args.half_duplex,
               interrupt=not args.no_interrupt, threshold=args.threshold,
               silence_ms=args.silence_ms, volume=args.volume,
               pace_factor=args.pace_factor, onset_guard_ms=args.onset_guard_ms,
               aec=not args.aec_off, voice=args.voice, url=args.url)

    frame = BrilliantMsg()
    session = None
    sampler = None
    started_audio = False
    try:
        if args.name:
            await frame.ble.connect(
                name=args.name,
                data_response_handler=frame._handle_data_response)
            await frame.ble.send_break_signal()
            await frame.ble.send_reset_signal()
            await frame.ble.send_break_signal()
        else:
            await frame.connect()
        if frame.ble.type != BrilliantDeviceType.HALO:
            print("This test requires a Halo")
            return

        await frame.print_short_text("VAD duplex test...")
        await frame.upload_stdlua_libs(lib_names=["data", "code",
                                                  "plain_text"])
        await frame.upload_frame_app(local_filename=args.lua)
        sampler = StatsSampler(frame, events)
        frame.attach_print_response_handler(sampler.on_print)
        await frame.start_frame_app()

        print(f"Connecting to {args.url} ...")
        async with connect(args.url, max_size=None) as ws:
            await ws.send(json.dumps(session_update(args)))

            frames_per_write = max(
                1, frame.max_lua_payload() // LC3_FRAME_BYTES)
            session = Session(frame, ws, frames_per_write, events,
                              duplex=not args.half_duplex,
                              pace_factor=args.pace_factor,
                              onset_guard_ms=args.onset_guard_ms,
                              barge_confirm_frames=args.barge_confirm_frames,
                              barge_rms=args.barge_rms)

            rx_audio = RxAudio(streaming=True)
            mic_queue = await rx_audio.attach(frame)

            await frame.send_message(START_PLAYBACK_MSG,
                                     TxCode(args.volume).pack())
            await frame.send_message(
                START_LISTENING_MSG,
                mic_start_payload(args.gain, aec=not args.aec_off,
                                  voice=args.voice))
            started_audio = True
            events.log("session_started")
            print("Speak to the Halo; talk over a reply to test barge-in. "
                  "Ctrl-C to finish.\n")

            stop = asyncio.Event()
            loop = asyncio.get_running_loop()
            for sig in (signal.SIGINT, signal.SIGTERM):
                try:
                    loop.add_signal_handler(sig, stop.set)
                except NotImplementedError:
                    pass
            tasks = [
                asyncio.create_task(session.receive()),
                asyncio.create_task(session.pump_mic(mic_queue)),
                asyncio.create_task(session.pump_speaker()),
                asyncio.create_task(sampler.run(args.stats_interval)),
            ]
            if args.auto_prompt:
                tasks.append(asyncio.create_task(
                    session.auto_prompt(args.auto_prompt,
                                        args.auto_interval)))
            if args.seconds:
                # completes the FIRST_COMPLETED wait after N s -> same clean
                # teardown as the SIGINT stop.set() (so the wav is saved)
                tasks.append(asyncio.create_task(asyncio.sleep(args.seconds)))
            done, pending = await asyncio.wait(
                set(tasks) | {asyncio.create_task(stop.wait())},
                return_when=asyncio.FIRST_COMPLETED)
            for t in pending:
                t.cancel()
            for t in done:
                if t.exception():
                    events.log("client_error", detail=repr(t.exception()))
            rx_audio.detach(frame)
    finally:
        if frame.is_connected():
            try:
                if started_audio and sampler is not None:
                    # session-true cumulative read while the app (and the
                    # audio sessions) are still alive
                    await sampler.sample()
                if started_audio:
                    await frame.send_message(STOP_LISTENING_MSG, b"")
                    await frame.send_message(STOP_PLAYBACK_MSG, b"")
                    await asyncio.sleep(0.3)
                await dump_aec_stats(frame, events)
            except Exception as e:
                print(f"stats dump failed: {e}")
            await frame.disconnect()

    if session is not None and session.mic_pcm:
        with wave.open(wav_path, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SR)
            w.writeframes(bytes(session.mic_pcm))
        if session.ref_pcm:
            with wave.open(f"duplex_ref_{ts}.wav", "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(SR)
                w.writeframes(bytes(session.ref_pcm))
        dur = len(session.mic_pcm) / 2 / SR
        print(f"\nwrote {wav_path} ({dur:.1f} s) + duplex_ref_{ts}.wav, "
              f"events in duplex_events_{ts}.jsonl")
        print("re-judge offline:  cd <speech-to-speech checkout> && "
              f"uv run python vad_gauntlet.py {wav_path}")


if __name__ == "__main__":
    asyncio.run(main())
