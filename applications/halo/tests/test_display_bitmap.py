# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = ["brilliant-ble>=3.1.1,<4"]
# ///
import asyncio
from brilliant_ble import BrilliantBle
import argparse

async def rgb_set_display_palette(b):
    
    frame_standard_palette = [
        0, 0, 0,          # 0: VOID 
        255, 255, 255,    # 1: WHITE 
        128, 128, 128,    # 2: GREY 
        255, 0, 0,        # 3: RED 
        255, 192, 203,    # 4: PINK
        101, 67, 33,      # 5: DARKBROWN
        150, 75, 0,       # 6: BROWN 
        255, 165, 0,      # 7: ORANGE 
        255, 255, 0,      # 8: YELLOW 
        0, 100, 0,        # 9: DARKGREEN
        0, 255, 0,        # 10: GREEN
        144, 238, 144,    # 11: LIGHTGREEN 
        25, 25, 112,      # 12: NIGHTBLUE
        0, 0, 205,        # 13: SEABLUE
        135, 206, 235,    # 14: SKYBLUE
        240, 248, 255     # 15: CLOUDBLUE 
    ]
    
    for i in range(16):
        start_index = i * 3
        r = frame_standard_palette[start_index]
        g = frame_standard_palette[start_index + 1]
        b_val = frame_standard_palette[start_index + 2]
        
        lua_command = f"frame.display.assign_color({i}, {r}, {g}, {b_val})"
        await b.send_lua(lua_command)
        await asyncio.sleep(0.1)

async def ycbcr_set_display_palette(b):
    # Set the palette back to the firmware default
    await b.send_lua("frame.display.assign_color_ycbcr(0, 0, 4, 4);") # VOID
    await b.send_lua("frame.display.assign_color_ycbcr(1, 15, 4, 4);") # WHITE
    await b.send_lua("frame.display.assign_color_ycbcr(2, 7, 4, 4);") # GREY
    await b.send_lua("frame.display.assign_color_ycbcr(3, 5, 3, 6);") # RED
    await b.send_lua("frame.display.assign_color_ycbcr(4, 9, 3, 5);") # PINK
    await b.send_lua("frame.display.assign_color_ycbcr(5, 2, 2, 5);") # DARKBROWN
    await b.send_lua("frame.display.assign_color_ycbcr(6, 4, 2, 5);") # BROWN
    await b.send_lua("frame.display.assign_color_ycbcr(7, 9, 2, 5);") # ORANGE
    await b.send_lua("frame.display.assign_color_ycbcr(8, 13, 2, 4);") # YELLOW
    await b.send_lua("frame.display.assign_color_ycbcr(9, 4, 4, 3);") # DARKGREEN
    await b.send_lua("frame.display.assign_color_ycbcr(10, 6, 2, 3);") # GREEN
    await b.send_lua("frame.display.assign_color_ycbcr(11, 10, 1, 3);") # LIGHTGREEN
    await b.send_lua("frame.display.assign_color_ycbcr(12, 1, 5, 2);") # NIGHTBLUE
    await b.send_lua("frame.display.assign_color_ycbcr(13, 4, 5, 2);") # SEABLUE
    await b.send_lua("frame.display.assign_color_ycbcr(14, 8, 5, 2);") # SKYBLUE
    await b.send_lua("frame.display.assign_color_ycbcr(15, 13, 4, 3);") # CLOUDBLUE
    print("Default palette set.")

async def get_display_palette(b, start_x=20, start_y=20):
    cols = 4  
    rows = 4 
    for row in range(rows):
        for col in range(cols):
            # index 0 (VOID) is a valid palette entry and is drawn like the
            # rest; it was previously skipped only to dodge a negative index.
            x = start_x + col * 60
            y = start_y + row * 60
            palette_index = row * cols + col
            lua_command = f"frame.display.bitmap({x}, {y}, 32, 2, {palette_index}, string.rep(string.char(1), 128))"
            await b.send_lua(lua_command)

async def full_display_palette(b):
    for palette_index in range(0, 16):
        lua_command = f"frame.display.bitmap(0, 0, 256, 2, {palette_index}, string.rep(string.char(1), 256/8*256))"
        await b.send_lua(lua_command)
        await asyncio.sleep(1)

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
    name = await b.connect(name=args.name, print_response_handler=lambda s: print(s))
    # Break main.lua before probing: its output otherwise lands in the reply
    # stream and await_print hands it back as the answer to these queries,
    # shifting every field of the banner below by one.
    await b.send_break_signal()
    fw = await b.send_lua("print(frame.FIRMWARE_VERSION)", await_print=True)
    tag = await b.send_lua("print(frame.GIT_TAG)", await_print=True)
    batt = await b.send_lua("print(frame.battery_level())", await_print=True)
    print(f"{name} | firmware {fw} | git {tag} | battery {batt}%")

    # Wake the display: it boots (and other tests leave it) in power-save.
    await b.send_lua("frame.display.power_save(false)")

    await b.send_lua("frame.display.clear(0x000000)")  # Black
    await asyncio.sleep(0.5)

    # await rgb_set_display_palette(b)
    # await asyncio.sleep(1)

    await ycbcr_set_display_palette(b)
    await asyncio.sleep(1)

    await get_display_palette(b)
    await asyncio.sleep(3)

    await b.send_lua("frame.display.clear(0x000000)")  # Black
    await asyncio.sleep(0.5)
    await full_display_palette(b)

    # When color_farmat is 0, data is full rgb
    await b.send_lua("frame.display.clear(0x000000)")  # Black
    await asyncio.sleep(0.5)
    await b.send_lua("frame.display.bitmap(40, 40, 100, 0, 0, string.rep('\\xff\\xff\\xff', 100*100))") 
    await asyncio.sleep(1)

    # When color_farmat is 2, data is the global palette index, Test black and white stripes
    await b.send_lua("frame.display.clear(0x000000)")  # Black
    await asyncio.sleep(0.5)
    await b.send_lua("frame.display.bitmap(0, 0, 256, 2, 0, string.rep('\\x00\\x00\\x00\\x00\\xff\\xff\\xff\\xff', 256/8*256/8))")
    await asyncio.sleep(1)

    # When color_farmat is 4, test 4 colored vertical stripes
    await b.send_lua("frame.display.clear(0x000000)")  # Black
    await asyncio.sleep(0.5)
    width = 256
    hight = 256
    byte_color = 4  # The number of colors contained in each byte(8/2)
    test_color_num = 4 # Test shows four colors
    rows = int(width/byte_color/test_color_num)
    print("rows:", rows)
    data1 = '\\x00' * rows
    data2 = '\\x55' * rows
    data3 = '\\xaa' * rows
    data4 = '\\xff' * rows
    data = data1 + data2 + data3 + data4
    count = int((width / byte_color) * (hight / rows / test_color_num))
    print("count:", count)
    lua_command = f"frame.display.bitmap(0, 0, {width}, 4, 7, string.rep('{data}', {count}))"
    await b.send_lua(lua_command)
    await asyncio.sleep(3)

    # When color_farmat is 16, test 16 colored horizontal stripes
    await b.send_lua("frame.display.clear(0x000000)")  # Black
    await asyncio.sleep(0.5)
    width = 256
    hight = 256
    byte_color = 2  # The number of colors contained in each byte(8/4)
    test_color_num = 16 # Test shows four colors
    data_list = ['\\x00', '\\x11', '\\x22', '\\x33',
                 '\\x44', '\\x55', '\\x66', '\\x77',
                 '\\x88', '\\x99', '\\xaa', '\\xbb',
                 '\\xcc', '\\xdd', '\\xee', '\\xff',
    ]
    line = int(hight/test_color_num)
    rows = int(width/byte_color*line)
    print("rows:", rows)
    print("line:", line)
    for i in range(0, 16):
        data = data_list[i]
        lua_command = f"frame.display.bitmap(0, {i*line}, {width}, 16, 0, string.rep('{data}', {rows}))"
        await b.send_lua(lua_command)
    await asyncio.sleep(3)

    # Use custom color palette, example:  displays 80x80 green square
    await b.send_lua("frame.display.clear(0x000000)")  # Black
    await asyncio.sleep(0.5)
    params = "{ \
                palette_data = \"\\xFF\\x00\\x00\\x00\\xFF\\x00\", \
                x_scale = 1,     \
                y_scale = 1,    \
            }"
    lua_command = f"frame.display.bitmap(40, 40, 80, 2, 0, string.rep('\\xff', 80/8 * 80), {params})"
    await b.send_lua(lua_command)
    await asyncio.sleep(1)

    # Test x_scale and y_scale, example:  displays 160x160 green square
    await b.send_lua("frame.display.clear(0x000000)")  # Black
    await asyncio.sleep(0.5)
    params = "{ \
                palette_data = \"\\xFF\\x00\\x00\\x00\\xFF\\x00\", \
                x_scale = 20,     \
                y_scale = 20,    \
            }"
    lua_command = f"frame.display.bitmap(40, 40, 8, 2, 0, string.rep('\\xff', 8/8 * 8), {params})"
    await b.send_lua(lua_command)
    await asyncio.sleep(1)

    # Leave the display as the firmware boots it: cleared and in power-save,
    # and resume the app we interrupted with the break signal.
    await b.send_lua("frame.display.clear(0x000000)")
    await b.send_lua("frame.display.power_save(true)")
    await b.send_reset_signal()

    # Disconnect Bluetooth
    await b.disconnect()

asyncio.run(main())
