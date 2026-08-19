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
    
    
    # Wake the display first: it boots (and other tests leave it) in
    # power-save, and show() alone does not bring it up.
    await b.send_lua("frame.display.power_save(false)")

    # --- Brightness test ---
    await b.send_lua("frame.display.show(true)")
    await asyncio.sleep(1.0)

    # --- Basic clear operations ---
    await b.send_lua("frame.display.clear(0x000000)")  # Black
    await asyncio.sleep(0.5)

    await b.send_lua("frame.display.clear(0xFF0000)")  # Red
    await asyncio.sleep(1)
    
    await b.send_lua("frame.display.clear(0x00FF00)")  # Green
    await asyncio.sleep(1)
    
    await b.send_lua("frame.display.clear(0x0000FF)")  # Blue
    await asyncio.sleep(1)

    await b.send_lua("frame.display.clear(0x000000)")  # Clear again before drawing

    # --- Text rendering ---
    # The display is 256x256 and glyphs advance 8px per character at 8px, so
    # 'The quick brown fox jumped' (26 chars = 208px) must start at x=1 to fit.
    await b.send_lua("frame.display.set_font(0)")  # Default font ID 0
    await b.send_lua("frame.display.text('Hello Frame!', 50, 50, 0xFFFFFF)")
    await b.send_lua("frame.display.text('The quick brown fox jumped', 1, 150, 0xFFFFFF)")
    await b.send_lua("frame.display.text('over the lazy dog.', 50, 200, 0xFFFFFF)")

    # Change font and scaling: bold at 2x scale (16px effective).
    # 'Big Bold!' is 9 chars = 144px at 16px; 32px would be 288px and overrun.
    await b.send_lua("frame.display.set_font(1, 8, 2)")  # DogicaBold, 8px, 2x
    await b.send_lua("frame.display.text('Big Bold!', 30, 100, 0x00FF00)")
    # set_font is global state: reset it so everything below renders at 8px.
    await b.send_lua("frame.display.set_font(0, 8, 1)")

    # # --- Get font list from Lua (for debugging or dynamic UI) ---
    # font_list = await b.send_lua("return frame.display.get_font_list()")
    # print("Font List:", font_list)

    # --- Drawing primitives ---
    await b.send_lua("frame.display.set_pixel(10, 10, 0x00FFFF)")  # Cyan pixel

    await b.send_lua("frame.display.line(20, 20, 100, 100, 0xFFFF00)")  # Yellow line

    # Filled and outlined rectangles
    await b.send_lua("frame.display.rect(120, 20, 60, 40, 0xFF00FF, true)")   # Filled magenta rect
    await b.send_lua("frame.display.rect(120, 80, 60, 40, 0xFF00FF, false)")  # Outlined

    # Filled and outlined circles
    await b.send_lua("frame.display.circle(200, 50, 20, 0x00FF00, true)")   # Green filled
    await b.send_lua("frame.display.circle(200, 100, 20, 0x00FF00, false)") # Green outline

    # Polygon (triangle)
    await b.send_lua("frame.display.polygon({160,160, 170,180, 150,180}, 0xFF8800)")

    # Draw individual character (ASCII code for 'A') — below the sentence at
    # y=200 rather than on top of it.
    await b.send_lua("frame.display.char(string.byte('A'), 50, 220, 0xFFFFFF)")

    # set_brightness takes a gain step from -2 to 2, not a percentage.
    for brightness in [-2, -1, 1, 2, 0]:
        await b.send_lua(f"frame.display.set_brightness({brightness})")
        await asyncio.sleep(0.5)

    # --- Display dimension test ---
    await b.send_lua("print('Display width:', frame.display.width())")
    await b.send_lua("print('Display height:', frame.display.height())")

    # --- Display pan test ---
    # y=130 is a free band (Big Bold ends at y=116, the fox line starts at 150).
    await b.send_lua("frame.display.text('Testing Pan', 100, 130, 0xFFFFFF)")
    await asyncio.sleep(1)
    
    # Pan to top-left
    await b.send_lua("frame.display.set_pan(-50, -50)")
    await asyncio.sleep(1)
    
    # Pan to bottom-right
    await b.send_lua("frame.display.set_pan(50, 50)")
    await asyncio.sleep(1)
    
    # Return to center
    await b.send_lua("frame.display.set_pan(0, 0)")
    await asyncio.sleep(1)
    
    # Get current pan
    await b.send_lua("local x, y = frame.display.get_pan(); print('Pan:', x, y)")

    # --- Power saving test ---
    await b.send_lua("frame.display.power_save(true)")
    await asyncio.sleep(5.0)

    await b.send_lua("frame.display.power_save(false)")
    await b.send_lua("frame.display.show(true)")
    await asyncio.sleep(1.0)

    # Leave the display as the firmware boots it: cleared and in power-save.
    await b.send_lua("frame.display.clear(0x000000)")
    await b.send_lua("frame.display.power_save(true)")

    # Resume the app we interrupted with the break signal.
    await b.send_reset_signal()

    # Disconnect Bluetooth
    await b.disconnect()

asyncio.run(main())
