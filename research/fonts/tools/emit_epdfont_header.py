#!/usr/bin/env python3
"""Emits EpdFontData C headers for the PaperS3's two SCREEN sizes (see
the project README, "SCREEN modes"), bypassing fontconvert.py
entirely (it only accepts FreeType-loadable font files -- TTF/OTF/BDF --
not raw pixel arrays like the ones this project's own Unscii resizer
produces).

Reuses generate_screen_fonts.py's actual glyph pipeline -- UnsciiScreenFont
(area-coverage resize + stem-width cap + the ç/Ç fix) for both sizes, since
both are non-integer scales of unscii-16 (1.375x and 2.75x) -- so the
firmware fonts are pixel-identical to the same pipeline validated on the
X4's own 64-col/80-col sizes, not a second, potentially-diverging
implementation.

Format matches editor/lib/EpdFont/builtinFonts/*.h exactly (verified
against EpdFontData.h and the pixel-placement math in GfxRenderer.cpp's
drawText()/renderChar()). Two steps, not one -- drawText() itself adds
the font's ascender to y *before* renderChar ever sees it:

    yPos    = y + fontData.ascender          // GfxRenderer::drawText()
    screenX = x    + glyph.left + glyphX     // GfxRenderer::renderChar()
    screenY = yPos - glyph.top  + glyphY

Every glyph here is the *entire* fixed-size cell (true monospace, not
proportionally trimmed like the project's prose fonts), so every glyph
gets identical left=0, top=0, width=cell_w, height=cell_h. For `y` in
drawText(fontId, x, y, ...) to land on the pixel row of the *top* of the
cell (no baseline-offset math needed by callers), ascender must be 0,
matching top=0 -- NOT cell_h, which an earlier (X4-era) version of this
script emitted, and which pushed every drawn character down by exactly
one full cell height (the cursor, drawn with fillRect() directly, doesn't
go through drawText() at all, so it stayed put -- that mismatch was the
tell). 1-bit packing (not the 2-bit grayscale mode the prose fonts use),
MSB-first, packed as one continuous bitstream across the whole glyph (NOT
per-row byte-aligned -- see pack_bits_contiguous()'s docstring for why that
distinction matters and what it broke before this was fixed).

advanceX is 12.4 fixed-point (4 fractional bits), NOT whole pixels -- see
EpdFontData.h's fp4 namespace and GfxRenderer.cpp's fp4::toPixel() call
sites. This is the one field that changed shape between the X4-era
EpdFontData this pipeline was written against and the version this repo's
EpdFont/GfxRenderer was carried from (crosspoint-reader-m5papers3): the
X4-era headers emitted advanceX as whole pixels, which reads as 1/16 of
the intended cell width under the new renderer -- compiles clean, renders
wrong, and there is no error to catch it. Every emitted advanceX here is
`cell_w << 4` for exactly that reason; do not "simplify" it back to
cell_w.

Usage:
    python3 emit_epdfont_header.py
writes both headers directly into
../../../editor/lib/EpdFont/builtinFonts/.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from generate_screen_fonts import UnsciiScreenFont  # noqa: E402

SRC = os.path.join(os.path.dirname(__file__), "..", "src")
OUT_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "..",
                        "editor", "lib", "EpdFont", "builtinFonts")


def pack_bits_contiguous(flat_bits):
    """flat_bits: row-major 0/1 values for the WHOLE glyph (width*height long),
    packed as one continuous MSB-first bitstream -- NOT per-row byte-aligned.

    This has to match GfxRenderer.cpp's renderCharImpl exactly: it tracks a
    single running `pixelPosition` across the entire glyph (`pixelPosition =
    glyphY * width + glyphX`, incremented once per pixel with no reset at row
    boundaries) and indexes the bitmap as `bitmap[pixelPosition >> 3]` -- i.e.
    zero padding between rows, padding only at the very end of the last byte.

    An earlier version of this function packed each row into its own
    ceil(width/8) bytes (padding every row out to a byte boundary). For any
    width that is a multiple of 8 that's identical to this scheme, which is
    exactly why 16x32/24x48 (the X4's SCREEN 0/1) always looked fine while
    12x24/10x20 (SCREEN 2/3) were quietly garbled from the second row of
    every glyph onward -- confirmed illegible on real X4 hardware, and
    reproduced here on the PaperS3's 11x22/22x44 fonts before this fix.
    """
    nbytes = (len(flat_bits) + 7) // 8
    value = 0
    for b in flat_bits:
        value = (value << 1) | (1 if b else 0)
    value <<= nbytes * 8 - len(flat_bits)
    return [(value >> (8 * (nbytes - 1 - i))) & 0xFF for i in range(nbytes)]


def build_intervals(codepoints):
    codepoints = sorted(codepoints)
    intervals = []
    start = prev = codepoints[0]
    for cp in codepoints[1:]:
        if cp == prev + 1:
            prev = cp
            continue
        intervals.append((start, prev))
        start = prev = cp
    intervals.append((start, prev))
    return intervals


def emit_header(font, cell_w, cell_h, name, source_note):
    codepoints = sorted(
        cp for cp in font.glyphs if (0x20 <= cp <= 0x7E) or (0xA0 <= cp <= 0xFF)
    )
    intervals = build_intervals(codepoints)
    for first, last in intervals:
        for cp in range(first, last + 1):
            assert cp in font.glyphs, f"{name}: gap at U+{cp:04X}"

    bitmap_bytes = bytearray()
    glyph_entries = []  # (width, height, advanceX, left, top, dataLength, dataOffset)
    for cp in codepoints:
        bits = font.get_cell_bits(chr(cp))
        glyph_offset = len(bitmap_bytes)
        flat_bits = [b for row in bits for b in row]
        bitmap_bytes.extend(pack_bits_contiguous(flat_bits))
        data_length = len(bitmap_bytes) - glyph_offset
        glyph_entries.append((cell_w, cell_h, cell_w << 4, 0, 0, data_length, glyph_offset))

    out = []
    out.append("// Generated by research/fonts/tools/emit_epdfont_header.py -- do not hand-edit.")
    out.append(f"// {source_note}")
    out.append("// Public domain / CC0 (Unscii, viznut) -- see research/fonts/src/unscii-LICENSE.")
    out.append("#pragma once")
    out.append("#include <cstdint>")
    out.append('#include "EpdFontData.h"')
    out.append("")

    out.append(f"static const uint8_t {name}Bitmaps[{len(bitmap_bytes)}] = {{")
    for i in range(0, len(bitmap_bytes), 16):
        chunk = bitmap_bytes[i:i + 16]
        out.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    out.append("};")
    out.append("")

    out.append(f"static const EpdGlyph {name}Glyphs[{len(glyph_entries)}] = {{")
    for w, h, adv, left, top, dlen, doff in glyph_entries:
        out.append(f"    {{ {w}, {h}, {adv}, {left}, {top}, {dlen}, {doff} }},")
    out.append("};")
    out.append("")

    out.append(f"static const EpdUnicodeInterval {name}Intervals[{len(intervals)}] = {{")
    running_offset = 0
    for first, last in intervals:
        out.append(f"    {{ 0x{first:X}, 0x{last:X}, {running_offset} }},")
        running_offset += last - first + 1
    out.append("};")
    out.append("")

    out.append(f"static const EpdFontData {name} = {{")
    out.append(f"    {name}Bitmaps,")
    out.append(f"    {name}Glyphs,")
    out.append(f"    {name}Intervals,")
    out.append(f"    {len(intervals)},")
    out.append(f"    {cell_h},  // advanceY")
    out.append("    0,  // ascender -- MUST be 0 to match glyph.top=0, see module docstring")
    out.append("    0,   // descender")
    out.append("    false,  // is2Bit")
    out.append("};")

    return "\n".join(out) + "\n", len(codepoints), len(intervals), len(bitmap_bytes)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    u16 = os.path.join(SRC, "unscii-16.hex")

    jobs = [
        ("unscii_11x22", UnsciiScreenFont(u16, 8, 16, 11, 22), 11, 22,
         "Portrait-era SCREEN 1 (48-col): area-coverage resize (1.375x) + stem-width cap + "
         "cedilla fix. Superseded by the landscape sizes below, kept generated in case portrait "
         "comes back for some other mode."),
        ("unscii_22x44", UnsciiScreenFont(u16, 8, 16, 22, 44), 22, 44,
         "Portrait-era SCREEN 0 (24-col): area-coverage resize (2.75x) + stem-width cap + "
         "cedilla fix. See unscii_11x22's note above."),

        # Landscape (960x540), same 4-tier column scheme and the same
        # FONT_SCREEN_MONO_0..3 numbering the X4's config.h used. All four
        # column counts divide 960 exactly (960 = 2^6*3*5); only 48-col and
        # 80-col leave a row remainder (960x540's short axis, 540 = 2^2*3^3*5,
        # isn't a clean multiple of every 2x cell height), split as an equal
        # top/bottom margin the same way the X4's own README documents for
        # its own non-exact SCREEN modes: "no border drawn, just empty panel."
        ("unscii_30x60", UnsciiScreenFont(u16, 8, 16, 30, 60), 30, 60,
         "SCREEN 0 landscape (32-col): area-coverage resize (3.75x) + stem-width cap + cedilla "
         "fix. 960/30=32 cols, 540/60=9 rows -- both exact."),
        ("unscii_20x40", UnsciiScreenFont(u16, 8, 16, 20, 40), 20, 40,
         "SCREEN 1 landscape (48-col, matches the X4's own default column count): "
         "area-coverage resize (2.5x) + stem-width cap + cedilla fix. 960/20=48 cols exact; "
         "540/40=13.5 -> 13 rows (520px), 10px margin top and bottom."),
        ("unscii_15x30", UnsciiScreenFont(u16, 8, 16, 15, 30), 15, 30,
         "SCREEN 2 landscape (64-col): area-coverage resize (1.875x) + stem-width cap + cedilla "
         "fix. 960/15=64 cols, 540/30=18 rows -- both exact, the full 960x540 panel with zero "
         "margin on either axis. This is main.cpp's current bring-up default -- confirmed "
         "legible on hardware before the other three landscape sizes were even generated."),
        ("unscii_12x24", UnsciiScreenFont(u16, 8, 16, 12, 24), 12, 24,
         "SCREEN 3 landscape (80-col): area-coverage resize (1.5x) + stem-width cap + cedilla "
         "fix. 960/12=80 cols exact; 540/24=22.5 -> 22 rows (528px), 6px margin top and bottom. "
         "Still comfortably above this project's own 10x20 'smallest still readable' floor."),
    ]

    for name, font, cell_w, cell_h, note in jobs:
        text, n_glyphs, n_intervals, n_bytes = emit_header(font, cell_w, cell_h, name, note)
        out_path = os.path.join(OUT_DIR, f"{name}.h")
        with open(out_path, "w") as f:
            f.write(text)
        print(f"wrote {name}.h: {n_glyphs} glyphs, {n_intervals} interval(s), {n_bytes} bitmap bytes")


if __name__ == "__main__":
    main()
