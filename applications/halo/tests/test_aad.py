# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = ["brilliant-ble>=3.1.1,<4"]
# ///
import asyncio
from brilliant_ble import BrilliantBle
import argparse

async def main():
    parser = argparse.ArgumentParser(
        description="Connect to a Halo/Frame device over BLE and run this test."
    )
    parser.add_argument(
        "--name",
        default=None,
        help='exact BLE device name, e.g. "Halo AB" or "Frame 4F"; defaults to the nearest device',
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=60.0,
        help="seconds to run for; 0 runs until interrupted (default: 60)",
    )
    args = parser.parse_args()

    b = BrilliantBle()

    # Connect to the device
    name = await b.connect(name=args.name, print_response_handler=lambda s: print(s))
    # Break main.lua before probing: its output otherwise lands in the reply
    # stream and await_print hands it back as the answer to these queries,
    # shifting every field of the banner below by one.
    await b.send_break_signal()
    fw = await b.send_lua("print(frame.FIRMWARE_VERSION)", await_print=True)
    tag = await b.send_lua("print(frame.GIT_TAG)", await_print=True)
    batt = await b.send_lua("print(frame.battery_level())", await_print=True)
    print(f"{name} | firmware {fw} | git {tag} | battery {batt}%")

    try:
        # Register the aad callback to print a message when triggered
        await b.send_lua(
            "frame.microphone.aad_callback(function() print('Acoustic Activity Detected!') end)"
        )

        print("Waiting for acoustic activity detection...")

        loop = asyncio.get_running_loop()
        deadline = None if args.duration <= 0 else loop.time() + args.duration
        while deadline is None or loop.time() < deadline:
            await asyncio.sleep(1)
    except KeyboardInterrupt:
        print("\nStopping test...")
    finally:
        # Unregister, so acoustic detection is not left running.
        try:
            await b.send_lua("frame.microphone.aad_callback(nil)")
        except Exception as e:
            print(f"could not clear aad callback: {e}")
        await b.send_reset_signal()
        await b.disconnect()

asyncio.run(main())