# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = ["brilliant-ble>=3.1.1,<4", "aioconsole"]
# ///
from aioconsole import ainput
from brilliant_ble import BrilliantBle
from halo_device_file import preserve_main_lua
import argparse
import asyncio
import time

image_buffer = b""
image_suffix = 1


def receive_data(data):
    global image_buffer
    global image_suffix
    print(
        f"Received {str(len(data))} bytes - {str(len(image_buffer))} total - {str(len(data)-1)} image"
    )
    if len(data) == 1:
        with open(f"test_camera_image_{image_suffix}.jpg", "wb") as f:
            print(f"Image {image_suffix} - Received {str(len(image_buffer)-1)} bytes")
            f.write(image_buffer)
            image_buffer = b""
            image_suffix += 1
        return
    image_buffer += data[1:]


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


    lua_script = """

    function transfer()
        while frame.camera.image_ready() == false do
            -- wait
        end

        while true do
            local i = frame.camera.read(frame.bluetooth.max_length() - 1)
            if (i == nil) then
                break
            else
                while true do
                    if pcall(frame.bluetooth.send, '0' .. i) then
                        break
                    end
                end
            end
        end

        while true do
            if pcall(frame.bluetooth.send, '0') then
                break
            end
        end
    end

    frame.camera.power_save(false)

    frame.camera.capture { resolution = 640, quality = 'LOW' }; transfer()

    print("Done - Press enter to finish")
    """

    # Connect to bluetooth and upload file
    b = BrilliantBle()
    name = await b.connect(
        name=args.name,
        print_response_handler=lambda s: print(s),
        data_response_handler=receive_data,
    )
    fw = await b.send_lua("print(frame.FIRMWARE_VERSION)", await_print=True)
    tag = await b.send_lua("print(frame.GIT_TAG)", await_print=True)
    batt = await b.send_lua("print(frame.battery_level())", await_print=True)
    print(f"{name} | firmware {fw} | git {tag} | battery {batt}%")

    print("Uploading script")

    # This test has to be main.lua (the reset path runs whatever is there), so
    # keep the device's own application and put it back afterwards. Previously
    # this script was left installed as main.lua permanently, so it ran at every
    # boot from then on.
    async with preserve_main_lua(b):
        await b.upload_file_from_string(lua_script, "main.lua")
        await b.send_reset_signal()

        # Wait until a keypress
        print(
            "Upload finished. Press enter to start capture."
        )
        await ainput("")

        await b.send_break_signal()

    await b.disconnect()


loop = asyncio.new_event_loop()
loop.run_until_complete(main())
