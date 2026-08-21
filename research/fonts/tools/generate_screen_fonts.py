#!/usr/bin/env python3
"""Regenerates every bitmap in research/fonts/previews/ from research/fonts/src/.

Final decision (see docs/DEVELOPMENT_LOG.md): Unscii is the default font for
SCREEN 0-3, in four cell sizes matching 32/48/64/80 columns on the X4's
800px-wide landscape panel. Terminus Bold is kept as a future settings-menu
alternative, same four column targets. Both output sets are rendered at
800x480 (landscape, native panel orientation) and then rotated 90 degrees
clockwise to 480x800, because that's what ships to the device -- see the
"why portrait" note in the README.

Requires Pillow (`pip install pillow`).

    python3 generate_screen_fonts.py

writes all 8 files into ../previews/, overwriting what's there.

--- Unscii: the interesting part ---

Unscii's native Latin glyphs only come in 8x8 and 8x16 -- both under this
project's 10x20px "smallest still readable" floor. 16x32 (48 columns) and
24x48 (32 columns) are clean 2x/3x nearest-neighbor upscales of the 8x16
source (integer ratios -- exact, no artifacts). 80 columns (10x20) and 64
columns (12x24) are *not* integer multiples of 8x16 (1.25x and 1.5x), so
naive nearest-neighbor upscaling picked an arbitrary source column to
duplicate or drop at each step -- stroke width flip-flopped between 1px
and 2px depending on a glyph's exact column, which is what made things
like '#', '*', '}', 'l', 'I', 'T' and the accented capitals look thin or
broken. AreaResampledHexFont fixes that by expanding the source bitmap to
the exact LCM canvas of both grids (an exact integer block-duplication,
so no approximation) and then shrinking back down by exact-block coverage
average -- every destination pixel's value reflects the source's actual
area coverage, not one arbitrarily-picked sample.

That alone overcorrected: a flat "any coverage lights the pixel" threshold
made already-solid strokes (I, T, |) balloon out to 4px wide. A flat "cap
every run at 3px" threshold, tried next, went too far the *other*
direction and conflated stroke *thickness* with stroke *length* -- it
chopped tall stems down to 3px *tall* instead of 3px *wide*. The one that
actually works: 25% coverage as the base threshold, then a *targeted*
post-pass (cap_stem_width) that only caps a row's horizontal run to 3px
when that run's width is close to the glyph's own *typical* (median) run
width -- i.e. it's clearly the stem rounding up by one pixel -- while
a row that's dramatically wider than typical (a serif bar, not a stem)
is left alone.

ç/Ç additionally get a small hand fix: the cedilla's hook bleeds into the
same output row as the letter's own last row during the area-coverage
resize, so a plain per-glyph pixel shift before scaling doesn't survive
cleanly. Fixed post-scale instead: render the plain c/C through the same
pipeline, diff row-by-row against ç/Ç to find exactly where the two
actually start to differ (not just where the plain letter's content
happens to end), and shift only those rows left by 1px.
"""
import math
import os
import statistics

from PIL import Image

SRC = os.path.join(os.path.dirname(__file__), "..", "src")
OUT = os.path.join(os.path.dirname(__file__), "..", "previews")

W, H = 800, 480  # landscape render canvas; rotated to 480x800 before saving

PANGRAM_LOWER = "the quick brown fox jumps over the lazy dog"
PANGRAM_UPPER = "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG"
SYMBOLS = "0123456789 !@#$%^&*()_+-=[]{}:;\"'<>,.?/\\|~`"
CHARSET = "".join(chr(c) for c in range(0x20, 0x7F))


# --------------------------------------------------------------- BDF -----
class BdfFont:
    """Parses a BDF bitmap font (used for Terminus)."""

    def __init__(self, path):
        self.glyphs = {}
        self.ascent = 0
        self.descent = 0
        self.bbox = (0, 0, 0, 0)
        self._parse(path)
        self.cell_w = self.bbox[0]
        self.cell_h = self.ascent + self.descent

    def _parse(self, path):
        with open(path, "r", encoding="latin-1") as f:
            lines = f.read().splitlines()
        cur_code = cur_bbx = None
        in_bitmap = False
        rows = []
        for line in lines:
            if line.startswith("FONTBOUNDINGBOX"):
                self.bbox = tuple(int(p) for p in line.split()[1:5])
            elif line.startswith("FONT_ASCENT"):
                self.ascent = int(line.split()[1])
            elif line.startswith("FONT_DESCENT"):
                self.descent = int(line.split()[1])
            elif line.startswith("ENCODING"):
                cur_code = int(line.split()[1])
            elif line.startswith("BBX"):
                cur_bbx = tuple(int(p) for p in line.split()[1:5])
            elif line.startswith("BITMAP"):
                in_bitmap = True
                rows = []
            elif line.startswith("ENDCHAR"):
                if cur_code is not None and cur_bbx is not None:
                    self.glyphs[cur_code] = (cur_bbx, rows)
                in_bitmap = False
                cur_code = cur_bbx = None
                rows = []
            elif in_bitmap:
                rows.append(int(line.strip(), 16) if line.strip() else 0)
        return

    def get_cell_bits(self, ch):
        cell = [[0] * self.cell_w for _ in range(self.cell_h)]
        g = self.glyphs.get(ord(ch))
        if not g:
            return cell
        (gw, gh, xoff, yoff), rows = g
        nbytes = (gw + 7) // 8
        top_row = self.ascent - yoff - gh
        for ry in range(gh):
            row_val = rows[ry] if ry < len(rows) else 0
            for rx in range(gw):
                bit = (row_val >> (nbytes * 8 - 1 - rx)) & 1
                if bit:
                    py, px = top_row + ry, xoff + rx
                    if 0 <= py < self.cell_h and 0 <= px < self.cell_w:
                        cell[py][px] = 1
        return cell


class ScaledBdfFont(BdfFont):
    """Nearest-neighbor integer upscale (used for TerminusBold's 32-col size,
    24x48 -- Terminus doesn't have a native size that large; 2x of 12x24)."""

    def __init__(self, path, factor):
        super().__init__(path)
        self.factor = factor
        self.base_cell_w, self.base_cell_h = self.cell_w, self.cell_h
        self.cell_w *= factor
        self.cell_h *= factor

    def get_cell_bits(self, ch):
        base = BdfFont.get_cell_bits(self, ch)
        f = self.factor
        cell = [[0] * self.cell_w for _ in range(self.cell_h)]
        for y in range(self.base_cell_h):
            for x in range(self.base_cell_w):
                if base[y][x]:
                    for dy in range(f):
                        for dx in range(f):
                            cell[y * f + dy][x * f + dx] = 1
        return cell


# --------------------------------------------------------------- HEX -----
class HexFont:
    """Parses a .hex bitmap font (used for Unscii)."""

    def __init__(self, path, cell_w, cell_h):
        self.cell_w, self.cell_h = cell_w, cell_h
        self.glyphs = {}
        with open(path, "r", encoding="ascii") as f:
            for line in f:
                line = line.strip()
                if not line or ":" not in line:
                    continue
                code_hex, data_hex = line.split(":")
                nbytes = len(data_hex) // 2
                width = 8 if nbytes == cell_h else (16 if nbytes == 2 * cell_h else 8)
                rows = [int(data_hex[j : j + 2], 16) for j in range(0, len(data_hex), 2)]
                self.glyphs[int(code_hex, 16)] = (width, rows)

    def get_cell_bits(self, ch):
        cell = [[0] * self.cell_w for _ in range(self.cell_h)]
        g = self.glyphs.get(ord(ch))
        if not g:
            return cell
        width, rows = g
        for ry in range(min(self.cell_h, len(rows))):
            row_val = rows[ry]
            for rx in range(min(width, self.cell_w)):
                if (row_val >> (width - 1 - rx)) & 1:
                    cell[ry][rx] = 1
        return cell


class ScaledHexFont(HexFont):
    """Clean integer upscale -- used for Unscii's 48-col (2x) and 32-col (3x)
    sizes. Exact block duplication, no threshold ambiguity at all."""

    def __init__(self, path, base_w, base_h, factor):
        super().__init__(path, base_w, base_h)
        self.factor = factor
        self.base_w, self.base_h = base_w, base_h
        self.cell_w, self.cell_h = base_w * factor, base_h * factor

    def get_cell_bits(self, ch):
        base = super().get_cell_bits(ch)
        f = self.factor
        cell = [[0] * self.cell_w for _ in range(self.cell_h)]
        for y in range(self.base_h):
            for x in range(self.base_w):
                if base[y][x]:
                    for dy in range(f):
                        for dx in range(f):
                            cell[y * f + dy][x * f + dx] = 1
        return cell


class AreaResampledHexFont(HexFont):
    """Non-integer-ratio upscale (Unscii's 80-col/10x20 and 64-col/12x24)
    via exact area-coverage: expand to the LCM of both grids, then shrink
    by exact-block coverage average at a 25% threshold. See module
    docstring for why 25% (not 50%, not >0)."""

    THRESHOLD_FRACTION = 0.25

    def __init__(self, path, base_w, base_h, target_w, target_h):
        super().__init__(path, base_w, base_h)
        self.base_w, self.base_h = base_w, base_h
        self.cell_w, self.cell_h = target_w, target_h
        self.lcm_w = base_w * target_w // math.gcd(base_w, target_w)
        self.lcm_h = base_h * target_h // math.gcd(base_h, target_h)
        self.expand_x, self.expand_y = self.lcm_w // base_w, self.lcm_h // base_h
        self.shrink_x, self.shrink_y = self.lcm_w // target_w, self.lcm_h // target_h

    def get_cell_bits(self, ch):
        base = super().get_cell_bits(ch)
        expanded = [[0] * self.lcm_w for _ in range(self.lcm_h)]
        for by in range(self.base_h):
            row = base[by]
            for bx in range(self.base_w):
                if row[bx]:
                    for dy in range(self.expand_y):
                        erow = expanded[by * self.expand_y + dy]
                        for dx in range(self.expand_x):
                            erow[bx * self.expand_x + dx] = 1
        cell = [[0] * self.cell_w for _ in range(self.cell_h)]
        block_area = self.shrink_x * self.shrink_y
        for y in range(self.cell_h):
            y0 = y * self.shrink_y
            for x in range(self.cell_w):
                x0 = x * self.shrink_x
                coverage = 0
                for dy in range(self.shrink_y):
                    erow = expanded[y0 + dy]
                    for dx in range(self.shrink_x):
                        coverage += erow[x0 + dx]
                if coverage >= self.THRESHOLD_FRACTION * block_area:
                    cell[y][x] = 1
        return cell


MAX_STEM_WIDTH = 3
STEM_OVERSHOOT_TOLERANCE = 1


def _row_runs(row):
    runs, w, x = [], len(row), 0
    while x < w:
        if row[x]:
            start = x
            while x < w and row[x]:
                x += 1
            runs.append((start, x))
        else:
            x += 1
    return runs


def cap_stem_width(bits):
    """Caps a row's run to MAX_STEM_WIDTH only when that run is a small
    overshoot of the glyph's own typical (median) run width -- i.e. clearly
    the stem, not a deliberately-wide serif/bar."""
    lengths = [end - start for row in bits for start, end in _row_runs(row)]
    if not lengths:
        return bits
    typical = statistics.median(lengths)
    out = [row[:] for row in bits]
    for y, row in enumerate(bits):
        for start, end in _row_runs(row):
            length = end - start
            if MAX_STEM_WIDTH < length <= typical + STEM_OVERSHOOT_TOLERANCE:
                excess = length - MAX_STEM_WIDTH
                trim_left = excess // 2
                for i in range(start, start + trim_left):
                    out[y][i] = 0
                for i in range(end - (excess - trim_left), end):
                    out[y][i] = 0
    return out


def fix_cedilla(font, ch, cell):
    plain_ch = "C" if ch == "Ç" else "c"
    plain = cap_stem_width(AreaResampledHexFont.get_cell_bits(font, plain_ch))
    first_diff = len(cell)
    for y in range(len(cell)):
        plain_row = plain[y] if y < len(plain) else [0] * len(cell[y])
        if cell[y] != plain_row:
            first_diff = y
            break
    out = [row[:] for row in cell]
    for y in range(first_diff, len(out)):
        out[y] = out[y][1:] + [0]
    return out


class UnsciiScreenFont(AreaResampledHexFont):
    """The final pipeline for Unscii's 80-col/64-col sizes: area-coverage
    resize + stem-width cap + the ç/Ç post-fix."""

    def get_cell_bits(self, ch):
        cell = cap_stem_width(super().get_cell_bits(ch))
        if ch in ("ç", "Ç"):
            cell = fix_cedilla(self, ch, cell)
        return cell


# ------------------------------------------------------------- canvas -----
def new_canvas():
    return Image.new("1", (W, H), 1)


def blit_cell(img, cell_bits, x, y, cell_w, cell_h):
    px = img.load()
    for ry in range(cell_h):
        if not (0 <= y + ry < H):
            continue
        row = cell_bits[ry]
        for rx in range(cell_w):
            if row[rx] and 0 <= x + rx < W:
                px[x + rx, y + ry] = 0


def column_ruler(total_cols):
    chars = [" "] * total_cols
    m = 5
    while m <= total_cols:
        s = str(m)
        start = m - len(s)
        for i, c in enumerate(s):
            chars[start + i] = c
        m += 5
    return "".join(chars)


def build_content_lines(content_cols, title):
    lines = [title[:content_cols]]
    while len(lines) <= 200:
        for i in range(0, len(CHARSET), content_cols):
            lines.append(CHARSET[i : i + content_cols])
        lines += ["", PANGRAM_LOWER, PANGRAM_UPPER, SYMBOLS, ""]
    return lines


def render_with_ruler(name, col_label, get_cell_bits, cell_w, cell_h, out_path):
    img = new_canvas()
    total_cols = max(1, W // cell_w)
    total_rows = H // cell_h
    content_cols = total_cols - 2

    title = f"{name} {col_label}col {cell_w}x{cell_h}"
    content_lines = build_content_lines(content_cols, title)

    for col, ch in enumerate(column_ruler(total_cols)):
        blit_cell(img, get_cell_bits(ch), col * cell_w, 0, cell_w, cell_h)

    for row_idx in range(2, total_rows + 1):
        y = (row_idx - 1) * cell_h
        if y + cell_h > H:
            break
        prefix = f"{row_idx:02d}"
        for col, ch in enumerate(prefix):
            blit_cell(img, get_cell_bits(ch), col * cell_w, y, cell_w, cell_h)
        line = content_lines[row_idx - 2] if (row_idx - 2) < len(content_lines) else ""
        for col, ch in enumerate(line):
            blit_cell(img, get_cell_bits(ch), (2 + col) * cell_w, y, cell_w, cell_h)

    img.rotate(-90, expand=True).save(out_path)
    print(f"wrote {os.path.basename(out_path)} (cell {cell_w}x{cell_h}, "
          f"landscape grid {total_cols}x{total_rows})")


def main():
    u16 = os.path.join(SRC, "unscii-16.hex")

    unscii_jobs = [
        ("32", ScaledHexFont(u16, 8, 16, 3)),
        ("48", ScaledHexFont(u16, 8, 16, 2)),
        ("64", UnsciiScreenFont(u16, 8, 16, 12, 24)),
        ("80", UnsciiScreenFont(u16, 8, 16, 10, 20)),
    ]
    for col_label, font in unscii_jobs:
        out_path = os.path.join(OUT, f"Unscii_{col_label}col_{font.cell_w}x{font.cell_h}.bmp")
        render_with_ruler("Unscii", col_label, font.get_cell_bits, font.cell_w, font.cell_h, out_path)

    terminus_bold_jobs = [
        ("32", ScaledBdfFont(os.path.join(SRC, "ter-u24b.bdf"), 2)),
        ("48", BdfFont(os.path.join(SRC, "ter-u32b.bdf"))),
        ("64", BdfFont(os.path.join(SRC, "ter-u24b.bdf"))),
        ("80", BdfFont(os.path.join(SRC, "ter-u20b.bdf"))),
    ]
    for col_label, font in terminus_bold_jobs:
        out_path = os.path.join(OUT, f"TerminusBold_{col_label}col_{font.cell_w}x{font.cell_h}.bmp")
        render_with_ruler("TerminusBold", col_label, font.get_cell_bits, font.cell_w, font.cell_h, out_path)


if __name__ == "__main__":
    main()
