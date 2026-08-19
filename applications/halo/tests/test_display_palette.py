# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = ["brilliant-ble>=3.1.1,<4"]
# ///
"""Structured visual check of palette indexing across the bitmap paths.

Every screen labels what SHOULD be on it, so a palette mix-up, channel swap
or off-by-one is visible at a glance instead of needing the source open:

  1. Global palette, labeled: each entry's name next to a bitmap swatch of
     that entry. Index 0 (VOID) is the transparency key and cannot be drawn
     by a bitmap; it is verified by overlaying index-0 data on a white bar
     (the bar must stay intact).
  2. palette_offset semantics: the same index-1 swatch pushed by offsets
     14 and 15. Offset 14 must show CLOUDBLUE (entry 15); offset 15 pushes
     past the palette and must be SKIPPED, leaving the white backing bar
     untouched (pre-fix firmware wrapped to entry 0 and painted it black).
  3. Format agreement: the same four-stripe pattern (WHITE RED GREEN
     SEABLUE) rendered via 1-bit+offset, 2-bit, 4-bit and RGB888 rows.
     All four rows must look identical; RED vs SEABLUE makes a red/blue
     channel swap in any one path jump out.
  4. Custom palette + scaling: pure red then pure green squares from a
     2-entry palette_data, at 1x and 8x scale.
  5. Primitive channel check: 'all of this row must be RED' drawn via
     rect, horizontal line, diagonal line, circle, set_pixel grid, and
     1-bit bitmap. Every item must be red; on firmware with the old
     canvas_set_pixel R/B swap the diagonal, circle and set_pixel
     entries showed BLUE (confirmed on hardware 2026-08-15).

Non-interactive except for eyes: exit code is always 0; judge on-screen.
"""
import asyncio
import argparse
from brilliant_ble import BrilliantBle

HOLD = 16.0  # seconds per screen

PALETTE_NAMES = [
    "VOID", "WHITE", "GREY", "RED", "PINK", "DARKBROWN", "BROWN", "ORANGE",
    "YELLOW", "DARKGREEN", "GREEN", "LIGHTGREEN", "NIGHTBLUE", "SEABLUE",
    "SKYBLUE", "CLOUDBLUE",
]

# 1-bit bitmap data: 0xFF bytes = every pixel index 1 (a solid swatch).
SOLID_1BIT = "string.rep('\\xff', {n})"


async def lua(b, cmd):
    await b.send_lua(f"pcall(function() {cmd} end)")


async def screen_header(b, title):
    await lua(b, "frame.display.clear(0x000000)")
    await lua(b, "frame.display.set_font(0, 8, 1)")
    await lua(b, f"frame.display.text('{title}', 1, 1, 0xFFFFFF)")


async def screen1_global_palette(b):
    """Two columns of 8: entry name + solid swatch of that entry."""
    await screen_header(b, "1: palette entries, labeled")
    for i in range(16):
        col = 0 if i < 8 else 1
        row = i % 8
        x_label = 1 + col * 128
        x_swatch = 89 + col * 128
        y = 17 + row * 24

        if i == 0:
            # VOID is the transparency key: overlay index-0 data on a white
            # bar; the bar must remain fully visible.
            await lua(b, f"frame.display.rect({x_swatch}, {y}, 32, 12, 0xFFFFFF, true)")
            await lua(b, f"frame.display.bitmap({x_swatch}, {y}, 32, 2, 0, "
                         f"string.rep('\\x00', 48))")
            await lua(b, f"frame.display.text('VOID(clear)', {x_label}, {y}, 0x808080)")
        else:
            # 1-bit data is index 1; palette_offset i-1 selects entry i.
            await lua(b, f"frame.display.bitmap({x_swatch}, {y}, 32, 2, {i - 1}, "
                         f"string.rep('\\xff', 48))")
            await lua(b, f"frame.display.text('{PALETTE_NAMES[i]}', {x_label}, {y}, 0xFFFFFF)")
    await asyncio.sleep(HOLD)


async def screen2_offset_semantics(b):
    await screen_header(b, "2: palette_offset")
    await lua(b, "frame.display.text('off 14 = CLOUDBLUE:', 1, 33, 0xFFFFFF)")
    await lua(b, "frame.display.bitmap(169, 33, 32, 2, 14, string.rep('\\xff', 48))")
    await lua(b, "frame.display.text('off 15 = skipped:', 1, 65, 0xFFFFFF)")
    await lua(b, "frame.display.text('(bar stays white; black = wrap bug)', 1, 81, 0x808080)")
    # White backing bar so a drawn-black wrap is distinguishable from skip.
    await lua(b, "frame.display.rect(169, 65, 32, 12, 0xFFFFFF, true)")
    await lua(b, "frame.display.bitmap(169, 65, 32, 2, 15, string.rep('\\xff', 48))")
    await asyncio.sleep(HOLD)


async def screen3_format_agreement(b):
    """Same WHITE|RED|GREEN|SEABLUE stripes through every decode path.

    Stripes are 16px wide, 64px total, drawn at x=97; each row is 12px tall.
    """
    await screen_header(b, "3: formats must match")
    # Row A: 1-bit, one bitmap per stripe (offset selects the entry).
    await lua(b, "frame.display.text('1-bit', 1, 33, 0xFFFFFF)")
    for s, entry in enumerate([1, 3, 10, 13]):
        await lua(b, f"frame.display.bitmap({97 + s * 16}, 33, 16, 2, {entry - 1}, "
                     f"string.rep('\\xff', 24))")

    # Row B: 2-bit indices 1..3 cover entries 1,3,10 via offsets; SEABLUE
    # needs its own call (2-bit reaches at most index 3 + offset).
    # Indices: 1,2,3 with offset 0 -> entries 1,2,3. To show the SAME colors
    # as row A we instead draw four separate 2-bit bitmaps of index 1 with
    # offsets 0,2,9,12.
    await lua(b, "frame.display.text('2-bit', 1, 49, 0xFFFFFF)")
    for s, entry in enumerate([1, 3, 10, 13]):
        await lua(b, f"frame.display.bitmap({97 + s * 16}, 49, 16, 4, {entry - 1}, "
                     f"string.rep('\\x55', 48))")

    # Row C: 4-bit, direct indices in one bitmap: 16px of each nibble.
    # (Lua needs parentheses to call a method on a string literal.)
    await lua(b, "frame.display.text('4-bit', 1, 65, 0xFFFFFF)")
    row = ("('\\x11'):rep(8) .. ('\\x33'):rep(8) .. "
           "('\\xaa'):rep(8) .. ('\\xdd'):rep(8)")
    await lua(b, f"local r = {row} frame.display.bitmap(97, 65, 64, 16, 0, r:rep(12))")

    # Row D: RGB888 direct: the same four colours as raw pixels.
    await lua(b, "frame.display.text('rgb888', 1, 81, 0xFFFFFF)")
    rgb = ("('\\xff\\xff\\xff'):rep(16) .. ('\\xff\\x00\\x00'):rep(16) .. "
           "('\\x00\\xff\\x00'):rep(16) .. ('\\x00\\x00\\xcd'):rep(16)")
    await lua(b, f"local r = {rgb} frame.display.bitmap(97, 81, 64, 0, 0, r:rep(12))")

    await lua(b, "frame.display.text('WHITE RED GREEN SEABLU', 97, 97, 0x808080)")
    await asyncio.sleep(HOLD)


async def screen4_custom_palette(b):
    await screen_header(b, "4: custom palette + scale")
    await lua(b, "frame.display.text('red 1x / green 8x:', 1, 33, 0xFFFFFF)")
    await lua(b, "frame.display.bitmap(1, 49, 16, 2, 0, string.rep('\\xff', 32), "
                 "{ palette_data = '\\x00\\x00\\x00\\xff\\x00\\x00' })")
    await lua(b, "frame.display.bitmap(33, 49, 8, 2, 0, string.rep('\\xff', 8), "
                 "{ palette_data = '\\x00\\x00\\x00\\x00\\xff\\x00', "
                 "x_scale = 8, y_scale = 8 })")
    await asyncio.sleep(HOLD)


async def screen5_channel_probe(b):
    """Everything on this screen must be RED. Blue = R/B channel swap."""
    await screen_header(b, "5: ALL of this must be RED")
    await lua(b, "frame.display.text('rect:', 1, 33, 0xFFFFFF)")
    await lua(b, "frame.display.rect(97, 33, 32, 12, 0xFF0000, true)")
    await lua(b, "frame.display.text('h-line:', 1, 57, 0xFFFFFF)")
    await lua(b, "frame.display.line(97, 61, 160, 61, 0xFF0000)")
    await lua(b, "frame.display.text('diagonal:', 1, 81, 0xFFFFFF)")
    await lua(b, "frame.display.line(97, 93, 160, 81, 0xFF0000)")
    await lua(b, "frame.display.text('circle:', 1, 113, 0xFFFFFF)")
    await lua(b, "frame.display.circle(113, 119, 10, 0xFF0000, false)")
    await lua(b, "frame.display.text('set_pixel:', 1, 145, 0xFFFFFF)")
    await lua(b, "for px = 97, 160 do for py = 145, 152 do "
                 "frame.display.set_pixel(px, py, 0xFF0000) end end")
    await lua(b, "frame.display.text('bitmap:', 1, 177, 0xFFFFFF)")
    await lua(b, "frame.display.bitmap(97, 177, 32, 2, 2, string.rep('\\xff', 48))")
    await asyncio.sleep(HOLD)


async def main():
    parser = argparse.ArgumentParser(
        description="Labeled visual verification of palette/bitmap indexing."
    )
    parser.add_argument("--name", default=None,
                        help='exact BLE device name, e.g. "Halo AB"')
    args = parser.parse_args()

    b = BrilliantBle()
    name = await b.connect(name=args.name, print_response_handler=lambda s: print(s))
    # Break main.lua before probing so its output cannot desync await_print.
    await b.send_break_signal()
    fw = await b.send_lua("print(frame.FIRMWARE_VERSION)", await_print=True)
    print(f"{name} | firmware {fw}")

    await b.send_lua("frame.display.power_save(false)")

    await screen1_global_palette(b)
    await screen2_offset_semantics(b)
    await screen3_format_agreement(b)
    await screen4_custom_palette(b)
    await screen5_channel_probe(b)

    # Leave the display as the firmware boots it and resume the app.
    await b.send_lua("frame.display.set_font(0, 8, 1)")
    await b.send_lua("frame.display.clear(0x000000)")
    await b.send_lua("frame.display.power_save(true)")
    await b.send_reset_signal()
    await b.disconnect()


asyncio.run(main())
