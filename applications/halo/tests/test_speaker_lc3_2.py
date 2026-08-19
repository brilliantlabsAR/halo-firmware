# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = ["brilliant-ble>=3.1.1,<4"]
# ///
import asyncio
from brilliant_ble import BrilliantBle
import argparse
import time

# Convert frame data to a Lua-compatible hexadecimal string
def bin2lua_hex(data: bytes) -> str:
    return '"' + ''.join(f'\\x{b:02x}' for b in data) + '"'

async def main():
    parser = argparse.ArgumentParser(
        description="Connect to a Halo/Frame device over BLE and run this test."
    )
    parser.add_argument(
        "--name",
        default=None,
        help='exact BLE device name, e.g. "Halo AB" or "Frame 4F"; defaults to the nearest device',
    )
    args = parser.parse_args()

    b = BrilliantBle()
    name = await b.connect(name=args.name)
    fw = await b.send_lua("print(frame.FIRMWARE_VERSION)", await_print=True)
    tag = await b.send_lua("print(frame.GIT_TAG)", await_print=True)
    batt = await b.send_lua("print(frame.battery_level())", await_print=True)
    print(f"{name} | firmware {fw} | git {tag} | battery {batt}%")

    # 1. Configure the speaker in LC3 mode
    await b.send_lua("frame.speaker.start{encoder='lc3', sample_rate=8000, "
                     "bit_depth=16, channels=1, bitrate=32000}")
    await b.send_lua("frame.speaker.volume(50)")
    await b.send_lua("function ble_event(d)frame.speaker.play(d)end")
    await b.send_lua("frame.bluetooth.receive_callback(ble_event)")
    await asyncio.sleep(1.0)
    # 2. Load LC3 audio frames. female_w1_8k_s16.lc3 is 8 kHz / 10 ms / 32 kbps
    #    => 40 bytes per frame, 2004 frames, 20.04 s.
    import os
    file_path = os.path.join(os.path.dirname(__file__), "female_w1_8k_s16.lc3")
    with open(file_path, "rb") as f:
        data = f.read()
    # Send whole LC3 frames only, as many as fit one BLE packet. play()
    # rejects any chunk that is not a multiple of the frame size
    # (lua_speaker.c:552), and oversized packets are dropped/rejected.
    LC3_FRAME = 40
    frames_per_packet = max(1, b.max_data_payload() // LC3_FRAME)
    frame_size = frames_per_packet * LC3_FRAME
    seconds_per_packet = frames_per_packet * 0.01
    print(f"packet {frame_size} B = {frames_per_packet} LC3 frames "
          f"({seconds_per_packet*1000:.0f} ms)")

    # 3. Send and play frame by frame
    for i in range(0, len(data), frame_size):
        frame = data[i:i + frame_size]
        lua_hex = bin2lua_hex(frame)
        await b.send_data(frame)
        await asyncio.sleep(seconds_per_packet)

    # 4. Stop playback
    #await b.send_lua("frame.speaker:stop()")
    await asyncio.sleep(1.0)

    await b.disconnect()

asyncio.run(main())
