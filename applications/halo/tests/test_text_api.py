# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = ["brilliant-ble>=3.1.1,<4"]
# ///
"""Exercise the Halo text API: placement, fonts, sizes/scales and colours.

Halo's text API differs from the original Frame's:
  - the logical display is 256x256, 1-based (not 640x400);
  - frame.display.text(str, x, y, colour) takes a 0xRRGGBB *integer*, not an
    options table -- there is no { color = 'NAME' } or { spacing = N };
  - glyph spacing is a property of the font, chosen with set_font(id, size,
    scale), where size is a multiple of 8;
  - the palette (assign_color/assign_color_ycbcr) applies to indexed *bitmap*
    drawing, not to text, so palette tests live in test_display_bitmap.py;
  - frame.display.show() is a no-op (firmware issue #27) and is not called.
"""
import asyncio
from brilliant_ble import BrilliantBle
import argparse

DISPLAY_W = 256
DISPLAY_H = 256

# Fonts are 8px pixel fonts; a size of N px advances N px per character.
BASE_PX = 8

# The firmware's default 16-entry palette, as RGB for direct text colouring.
PALETTE = [
    ("WHITE", 0xFFFFFF),
    ("GREY", 0x808080),
    ("RED", 0xFF0000),
    ("PINK", 0xFFC0CB),
    ("DARKBROWN", 0x654321),
    ("BROWN", 0x964B00),
    ("ORANGE", 0xFFA500),
    ("YELLOW", 0xFFFF00),
    ("DARKGREEN", 0x006400),
    ("GREEN", 0x00FF00),
    ("LIGHTGREEN", 0x90EE90),
    ("NIGHTBLUE", 0x191970),
    ("SEABLUE", 0x0000CD),
    ("SKYBLUE", 0x87CEEB),
    ("CLOUDBLUE", 0xF0F8FF),
]


async def corners(b):
    """Text in all four corners, inset by 1px from each edge."""
    text = "Test"
    w = len(text) * BASE_PX
    right = DISPLAY_W - w + 1
    bottom = DISPLAY_H - BASE_PX + 1
    await b.send_lua(f"frame.display.text('{text}', 1, 1)")
    await b.send_lua(f"frame.display.text('{text}', {right}, 1)")
    await b.send_lua(f"frame.display.text('{text}', 1, {bottom})")
    await b.send_lua(f"frame.display.text('{text}', {right}, {bottom})")


async def ascii_coverage(b):
    """Every printable ASCII glyph (0x20-0x7E) the fonts actually carry.

    The fonts cover ASCII only, so non-ASCII input (e.g. 'AOA' with umlauts)
    renders as missing glyphs -- deliberately not tested here.
    """
    y = 1
    for start in range(0x20, 0x7F, 32):
        chunk = "".join(chr(c) for c in range(start, min(start + 32, 0x7F)))
        # escape the Lua string delimiters and backslash
        esc = chunk.replace("\\", "\\\\").replace("'", "\\'")
        await b.send_lua(f"frame.display.text('{esc}', 1, {y})")
        y += BASE_PX * 2


async def sizes_and_scales(b):
    """Walk the font sizes, then the extra scale multiplier, on each font."""
    fonts = await b.send_lua(
        "local t = frame.display.get_font_list() "
        "local s = '' for i, n in pairs(t) do s = s .. i .. '=' .. n .. ' ' end "
        "print(s)",
        await_print=True,
    )
    print(f"fonts: {fonts}")

    for font_id in (0, 1):
        await b.send_lua("frame.display.clear(0x000000)")
        y = 1
        for size in (8, 16, 24, 32):
            await b.send_lua(f"frame.display.set_font({font_id}, {size})")
            await b.send_lua(f"frame.display.text('Size {size}', 1, {y})")
            y += size + 4
        await asyncio.sleep(2.00)

    # scale multiplies on top of size: size 8 x scale 2 == size 16
    await b.send_lua("frame.display.clear(0x000000)")
    await b.send_lua("frame.display.set_font(0, 8, 2)")
    await b.send_lua("frame.display.text('8px @ scale 2', 1, 1)")
    await b.send_lua("frame.display.set_font(0, 16, 1)")
    await b.send_lua("frame.display.text('16px @ scale 1', 1, 40)")
    await asyncio.sleep(2.00)

    await b.send_lua("frame.display.set_font(0, 8, 1)")


async def colors(b):
    """Each default-palette colour, drawn as a direct RGB text colour."""
    await b.send_lua("frame.display.clear(0x000000)")
    half = (len(PALETTE) + 1) // 2
    for i, (name, rgb) in enumerate(PALETTE):
        x = 1 if i < half else DISPLAY_W // 2
        y = 1 + (i % half) * (BASE_PX * 2)
        await b.send_lua(f"frame.display.text('{name}', {x}, {y}, 0x{rgb:06X})")


async def bad_arguments(b):
    """Bad input must raise a Lua error rather than silently misbehave."""
    checks = [
        ("frame.display.set_font(0, 12)", "size not a multiple of 8"),
        ("frame.display.set_font(99, 8)", "font id out of range"),
        ("frame.display.set_font(0, 8, 0)", "scale below 1"),
    ]
    failures = []
    for call, why in checks:
        # Print a single short token: printing pcall's own results would emit
        # "false<TAB><error message>", which arrives as several BLE
        # notifications and desynchronises await_print by one reply.
        resp = await b.send_lua(
            f"local ok = pcall(function() {call} end) "
            "print(ok and 'noerror' or 'raised')",
            await_print=True,
        )
        raised = resp is not None and resp.strip() == "raised"
        print(f"  {'ok  ' if raised else 'FAIL'} rejects {why}: {call}")
        if not raised:
            failures.append(why)
    await b.send_lua("frame.display.set_font(0, 8, 1)")
    return failures


async def main():
    parser = argparse.ArgumentParser(
        description="Connect to a Halo device over BLE and exercise the text API."
    )
    parser.add_argument(
        "--name",
        default=None,
        help='exact BLE device name, e.g. "Halo AB"; defaults to the nearest device',
    )
    args = parser.parse_args()

    b = BrilliantBle()

    name = await b.connect(name=args.name, print_response_handler=lambda s: print(s))
    # Break main.lua before probing: its output otherwise lands in the reply
    # stream and desynchronises every await_print below (banner and the
    # bad-argument checks alike).
    await b.send_break_signal()
    fw = await b.send_lua("print(frame.FIRMWARE_VERSION)", await_print=True)
    tag = await b.send_lua("print(frame.GIT_TAG)", await_print=True)
    batt = await b.send_lua("print(frame.battery_level())", await_print=True)
    print(f"{name} | firmware {fw} | git {tag} | battery {batt}%")

    await b.send_lua("frame.display.power_save(false)")
    await b.send_lua("frame.display.clear(0x000000)")

    await corners(b)
    await asyncio.sleep(2.00)

    await b.send_lua("frame.display.clear(0x000000)")
    await ascii_coverage(b)
    await asyncio.sleep(2.00)

    await sizes_and_scales(b)

    await colors(b)
    await asyncio.sleep(2.00)

    print("argument validation:")
    failures = await bad_arguments(b)

    await b.send_lua("frame.display.clear(0x000000)")
    await b.send_lua("frame.display.power_save(true)")

    # Resume the app we interrupted with the break signal.
    await b.send_reset_signal()

    await b.disconnect()

    if failures:
        raise SystemExit(f"argument validation failed: {', '.join(failures)}")


asyncio.run(main())
