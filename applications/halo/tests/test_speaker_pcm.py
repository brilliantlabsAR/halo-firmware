# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = ["brilliant-ble>=3.1.1,<4"]
# ///
import asyncio
import struct
from brilliant_ble import BrilliantBle
import argparse

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

    # 1. Configure the speaker in pcm mode
    await b.send_lua("frame.speaker.start{encoder='pcm', sample_rate=8000, "
                     "bit_depth=16, channels=1}")
    await b.send_lua("frame.speaker.volume(10)")


    import os
    # The tracked fixture is signed 8-bit, but the firmware only accepts
    # bit_depth=16 (lua_speaker.c:370) - feeding it the 8-bit bytes would play
    # as noise. Widen to signed 16-bit LE here rather than committing a second
    # derived asset.
    file_path = os.path.join(os.path.dirname(__file__), "female_w1_8k_s8.pcm")
    with open(file_path, "rb") as f:
        raw = f.read()
    data = bytearray()
    for byte in raw:
        data += struct.pack("<h", (byte - 256 if byte > 127 else byte) << 8)
    data = bytes(data)

    # send_audio() SILENTLY DROPS anything larger than the negotiated MTU
    # payload, so size each packet from the live connection. Keep it even:
    # 16-bit samples must not be split across packets.
    frame_size = (b.max_data_payload() // 2) * 2
    seconds_per_packet = frame_size / 2 / 8000
    print(f"packet {frame_size} B ({seconds_per_packet*1000:.1f} ms of audio)")

    # 3. Send and play frame by frame
    for i in range(0, len(data), frame_size):
        frame = data[i:i + frame_size]
        # lua_hex = bin2lua_hex(frame)
        # await b.send_lua(f"frame.speaker.play({lua_hex})")
        await b.send_audio(frame)
        # Pace to real time or the 8 KB device ring overflows and the
        # excess is dropped without feedback.
        await asyncio.sleep(seconds_per_packet)

    # 4. Stop playback
    await b.send_lua("frame.speaker.stop()")
    await asyncio.sleep(1.0)

    await b.disconnect()

asyncio.run(main())
