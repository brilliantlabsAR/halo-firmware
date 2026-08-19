#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9,<3.13"
# dependencies = ["brilliant-ble"]
# ///
"""Diagnose mic frame loss while the LC3 speaker path is decoding."""
import asyncio

from brilliant_ble import BrilliantBle

printed = []


async def main():
    b = BrilliantBle()
    await b.connect()  # nearest device
    b._user_print_response_handler = lambda s: printed.append(s)
    try:
        await b.send_break_signal()
        await asyncio.sleep(0.5)
        await b.send_lua("frame.microphone.diag('zero') print('z')",
                         await_print=True, timeout=10)
        await b.send_lua(
            "frame.microphone.aec(false) "
            "frame.microphone.start{encoder='lc3', sample_rate=16000, "
            "bitrate=32000, channels=1} "
            "enc_sil = string.rep('\\0', 40*6) cap=0 print('go')",
            await_print=True, timeout=10)
        printed.clear()
        await b.send_lua(
            "frame.speaker.start{encoder='lc3', sample_rate=16000, "
            "channels=1, duration=1000, bitrate=32000, volume=30} "
            "local t0=frame.time.utc() "
            "for i=1,80 do pcall(function() frame.speaker.play(enc_sil) end) "
            "local s=frame.microphone.read(4080) if s then cap=cap+#s end end "
            "local t1=frame.time.utc() frame.speaker.stop() "
            "for i=1,100 do local s=frame.microphone.read(4080) "
            "if s then cap=cap+#s end frame.sleep(0.01) end "
            "frame.microphone.stop() print(cap..' '..(t1-t0))",
            await_print=True, timeout=60)
        print("captured-LC3-bytes playloop-seconds:", printed[-1])
        printed.clear()
        await b.send_lua(
            "local s=frame.microphone.diag('stats') "
            "print(s.pdm_popped..' '..s.pdm_dropped..' '..s.pdm_overflows"
            "..' '..s.pdm_isrs..' '..s.pdm_stat_max)",
            await_print=True, timeout=10)
        print("pdm (popped dropped overflows isrs stat_max):", printed[-1])
    finally:
        await b.send_reset_signal()
        await b.disconnect()


asyncio.run(main())
