# MicroBASIC-PaperS3

A port of [MicroBASIC](https://github.com/fperuzzo72/MicroBASIC) — the "new"
1980s microcomputer that boots straight into a text-screen BASIC — from the
Xteink X4 to the **M5Stack PaperS3**.

The X4 is an ESP32-C3 with 380KB of RAM, a 1-bit 800×480 panel, and a 5-way
d-pad. The PaperS3 is an ESP32-S3 with 8MB of PSRAM, a 960×540 16-gray panel,
and **no physical navigation buttons at all**. The second of those is what
makes this a port rather than a new build target: every interaction has to be
rebuilt for touch.

## Sibling project

MicroBASIC is not a fork of this repo, and this isn't a fork of it either —
two separate repositories, each with its own history. The X4 is the full
version: it started life as a copy of the MicroWriter firmware and carries,
on top of BASIC, the prose editor, WiFi sync, a BLE keyboard, a file
browser, and the VC picker.

This port doesn't have those parts yet — but that's because it's a port in
progress, not because they're out of scope by design. They're expected to
land here eventually (the on-screen keyboard already has a working BLE
keyboard alongside it — see the status note below). That means the shared
surface below is going to grow, not stay stable, and the habit of carrying
a fix to both sides matters more over time, not less.

The two repositories stay separate either way: one tree serving both a
touch panel with an on-screen keyboard and a button panel with a BLE
keyboard would turn into conditional logic on top of conditional logic.

Four areas are the same code in both projects today, and a fix in one is a
fix in the other — this list will grow as `text_editor`, `file_manager`,
and `ui_renderer` get ported over, each one adding another surface a fix
has to cross:

| Area | What it is |
|---|---|
| `research/fonts/tools/` | font pipeline: resizing, stem-width capping, header emission |
| `editor/src/screen_editor.*` | the character grid, line wrapping, logical lines |
| `editor/src/tb_*` | interpreter integration and the runtime contract |
| `patches/tinybasic/` | the patch set over upstream |

This isn't theoretical. In a single session, this port surfaced four defects
that had been latent on the X4 for months — see `docs/DEVELOPMENT_LOG.md`
for the full account, including two spots where this port's fix diverged
from the X4's own. They showed up here first because a different panel size
exercises assumptions the X4's own hardware never questions: glyph widths
that aren't multiples of 8, a logical line wrapping into a different number
of physical rows. Porting turned out to be a test suite nobody would have
thought to write.

When the text editor itself gets ported, it'll bring undo, `.bak` discard,
and `ascii_fold` along with it — already done and confirmed on X4 hardware.

## Status

**Milestones 1-3 — hardware bring-up, real SCREEN fonts, on-screen
keyboard — done and confirmed on real hardware.** `editor/src/main.cpp` is a
bring-up program, not the firmware: it proves the EPD, the GT911 touch panel
and the SD card; fills the terminal grid with real glyphs so legibility could
be judged on the physical panel rather than assumed; and demonstrates a full
touch keyboard (`osk.cpp`/`osk.h`) by echoing typed, US-International-composed
text into that grid.

Landscape, not portrait: an early portrait build (540px wide) worked but felt
cramped — both the terminal text and the keyboard keys read as too small on
real hardware. Landscape (960×540, the panel's native orientation) gave both
the room they needed. The terminal defaults to SCREEN 2 (64-col, cell 15×30,
`unscii_15x30`) — 960/15=64 and 540/30=18 divide the panel exactly, zero
margin either axis. Font legibility survived one real bug along the way: the
first attempt at any of these SCREEN fonts came out garbled, traced to a real
packing bug in `emit_epdfont_header.py` (see "The font format changed
underneath the port" below), not a resolution limit. Fixed, and verified by
decoding the regenerated header with the renderer's own bit-reading algorithm
before ever trusting it on hardware again.

The on-screen keyboard (five main rows plus Esc alone above them, US-
International dead-key composition via `dead_keys.h`, a two-tier gray keycap
style, a full ANSI-ish layout with a physical-keyboard row stagger) went
through many rounds of real-hardware iteration — arrow cluster shape,
Enter's position and shape, key spacing and corner rounding, Shift-hint
placement — before landing on something confirmed to work well, limited only
by this panel's touch-reading precision, not by the keyboard's own design.

The X4 sources are carried in `editor/port-staging/` and move into
`editor/src/` one at a time as each is ported (`dead_keys.h` already has,
being self-contained). Nothing else there compiles for this board yet --
that is the point of staging them.

## Hardware layer

Hardware comes from [`freeink-sdk`](https://github.com/fperuzzo72/freeink-sdk)
(submodule, `m5papers3-support` branch), whose `M5PAPERS3` profile was
bench-tested on a physical unit — see `freeink-sdk/docs/m5papers3-support.md`
for the pin-by-pin CONFIRMED/PENDING breakdown. `GfxRenderer`, `EpdFont` and
`hal` are carried from
[crosspoint-reader-m5papers3](https://github.com/fperuzzo72/crosspoint-reader-m5papers3),
which already runs this panel; MicroBASIC's own copies were the older X4-era
versions with the panel size baked in as compile-time constants.

The GT911 `flipX`/`flipY` values (inherited from M5Paper v1.1 by analogy) are
**confirmed correct** via a real 4-corner-tap test on the physical device —
was listed unverified in the SDK's own profile, updated there too.

## Screen geometry — decided

Landscape, not portrait. An early portrait build (540px logical width, the
terminal stacked above a keyboard permanently docked below it) worked but
felt cramped on real hardware — both the terminal text and the keyboard keys
read as too small. Landscape (960×540) is this panel's native rotation=0
orientation (`GfxRenderer::LandscapeCounterClockwise`, confirmed correct on
hardware) and gives both the room they needed.

With the terminal no longer sharing the panel with a permanently docked
keyboard, it claims the **entire** 960×540 panel. The on-screen keyboard is a
**toggleable overlay** instead — a "KBD" button (top-right) shows/hides it
covering the bottom 300px (10 terminal rows) when needed. A paired BLE
keyboard is preferred when one's available and the on-screen keyboard is the
reserve you summon on demand, not something permanently eating screen space
— **BLE keyboard pairing is done and confirmed working**: a "BLE" status
button under the KBD toggle shows connection state at a glance and forces a
fresh pairing scan on tap; see the on-screen-keyboard status note below for
how automatic pairing/reconnect works.

### SCREEN modes

| Mode | Columns × Rows | Cell | Scale from unscii-16 |
|---|---|---|---|
| `SCREEN 0` | 32×8 | 30×60 | 3.75× |
| `SCREEN 1` | 48×12 | 20×40 | 2.5× — matches the X4's own default column count |
| `SCREEN 2` | 64×17 | 15×30 | 1.875× — **boots here** |
| `SCREEN 3` | 80×21 | 12×24 | 1.5× |

All four column counts from the X4's original scheme carry over unchanged
(960 = 2⁶×3×5, so every one of 32/48/64/80 divides it exactly — no column
margin in any mode, unlike the X4's own 800px panel). Row counts are one
lower than a straight 540px/cellH division across all four modes: the top
30px of the panel is reserved for the status bar (KBD/BLE and the
MENU/EDITOR/SYNC placeholders), so the terminal's own usable band is 510px,
not 540. `SCREEN 2` divides that exactly (17 rows, zero extra margin) — the
nicest fit of the four, since the bar's 30px height is itself a multiple of
its 30px cell height; the other three get a small centered top/bottom
margin within their 510px band the same way the X4's own non-exact modes
do. This device boots into
`SCREEN 2` (64-col) by default rather than the X4's `SCREEN 1` (48-col) —
deliberately different: the X4 defaults to 48-col because that reads best at
its panel size, but this panel has enough room that 64-col is the better
default here. All four fonts are generated and verified; only `SCREEN 2`'s is
currently wired into the renderer, since nothing in this bring-up program can
switch modes at runtime (no BASIC interpreter yet).

Every cell above is a non-integer rescale of unscii-16, the case
`AreaResampledHexFont` already exists to handle (the X4's own 10×20 and
12×24 sizes are 1.25× and 1.5× for the same reason). All comfortably clear
the readability floor this project established on the X4:

> Unscii's native Latin glyphs only come in 8x8 and 8x16 — both under this
> project's 10x20px "smallest still readable" floor.
>
> — `research/fonts/tools/generate_screen_fonts.py`

### The font format changed underneath the port

The X4's four `unscii_*.h` headers **cannot be carried across unmodified**,
and the failure is silent — they compile. Between the two lineages `EpdGlyph`
gained a fixed-point advance:

```c
uint8_t  advanceX;   // X4 era: whole pixels
uint16_t advanceX;   // now: 12.4 fixed-point, read via fp4::toPixel()
```

`GfxRenderer.cpp` reads it as 12.4 throughout, so an advance emitted in whole
pixels renders at 1/16 of its intended width. `emit_epdfont_header.py` needs
`advanceX = cell_w << 4`. Fixed, in `research/fonts/tools/emit_epdfont_header.py`.

The trailing `EpdFontData` fields added since (groups, kerning classes,
ligatures, glyph-miss handlers) are all safe to leave out: the headers use
positional aggregate initialization, so the new members value-initialize to
zero/`nullptr`, which is exactly "no compression, no kerning, no ligatures".
`ascender` must still be 0 to match `top=0` — `drawText()` still adds it.

A second, more serious bug was in the emitter, not the format: it packed
each bitmap **row** into its own byte-aligned bytes, while
`GfxRenderer.cpp`'s `renderCharImpl()` reads the glyph as **one continuous
bitstream** with no padding between rows. The two schemes are
byte-identical for any cell width that happens to be a multiple of 8 —
which is exactly why it went unnoticed on the X4 (SCREEN 0/1, widths 24/16)
while SCREEN 2/3 (widths 12/10) were quietly garbled from each glyph's
second row onward. This device's own 11×22/22×44 fonts (widths 11/22)
reproduced it immediately, confirmed by a photo showing crisp NotoSans UI
text next to visibly streaked mono glyphs in the same frame. Fixed via
`pack_bits_contiguous()` in the same emitter. Verify any future font header
against `renderCharImpl()`'s actual read loop directly (decode a few known
glyphs in Python using that exact bit indexing and render to a PNG) rather
than trusting that the resize algorithm being correct implies the final
packed bytes are — the two bugs above both compiled clean and looked
individually reasonable.

## Flashing

**Never `pio run -t upload`, and never write to `0x10000`.** That writes
bootloader.bin + the partition table + otadata + the app — not just the
app — silently replacing M5Launcher's own bootloader with one this
project's `platformio.ini` compiles. On this device that is not cosmetic:
the EPD only draws when running under **M5Launcher's own original
bootloader** — confirmed by direct A/B, same app code, same serial "success"
either way, panel dead under a freshly-compiled bootloader and working under
Launcher's. `0x10000` (`app0`) is also not "whatever's convenient" — it is
M5Launcher's actual code, the thing that runs on every boot and decides what
to load next; overwriting it breaks the picker, not just this build.

The safe recipe: build the app only, then write *just* `firmware.bin` into
an existing **app-type OTA partition that is not `app0`**, with plain
esptool — never bootloader.bin, partitions.bin, or otadata:

```bash
esptool.py --chip esp32s3 --port <port> --baud 921600 \
    write_flash <slot offset> .pio/build/m5papers3/firmware.bin
```

Read the live partition table first (`esptool read_flash 0x8000 0xC00` +
`gen_esp32part.py`) — offsets change whenever slots are added or removed.
As of 2026-08-21 the table is:

```
nvs       data  nvs      0x9000    16K
otadata   data  ota      0xd000     8K
phy_init  data  phy      0xf000     4K
app0      app   test     0x10000  1536K   <- M5Launcher, NEVER write here
coredump  data  coredump 0x190000   64K
crossp    app   ota_0    0x1a0000 5312K   <- currently holds THIS bring-up
                                             firmware, not the real reader
```

`crossp` is where this project's own testing has been landing (CrossPoint's
own binary is not currently in flash on the dev unit — restore it from
`~/Desktop/M5PaperS3-backup/` if the reader itself is needed again). After
flashing, power-cycle the physical button rather than triggering an esptool
soft reset — Launcher decides what to load on every boot and has an
intermittent bug where it doesn't always auto-load the last-used firmware,
needing a manual tap-through.

Full-flash backups of the dev unit (bootloader + partition table + every
app, restorable in one `write_flash 0x0 <backup>.bin`) live outside this
repo at `~/Desktop/M5PaperS3-backup/`, with their own `RESTAURAR.md`.

## What has to work

1. Text editor with the US-International keyboard layout and dead keys.
   TypeWriter and Clean screen modes are dropped; the standard mode only.
   **Dead-key composition itself is done and confirmed** — `dead_keys.h`
   (self-contained, no editor/BLE/wifi dependencies) is ported and wired into
   the bring-up's typing demo; the text editor it belongs in
   (`text_editor.cpp`) is still in `port-staging/`.
2. BASIC interpreter and screen editor.
3. File read/write, creating new `.txt` and `.bas` files.
4. WiFi and the file-transfer web server.
5. **Bluetooth keyboard — done and confirmed on hardware.** Auto-pairs with
   the first HID-advertising device seen (no pairing UI exists yet) and
   auto-reconnects to the saved bond on every boot; falls back to scanning
   again if the saved device doesn't answer for a while, or immediately on a
   tap of the "BLE" status button.
6. **On-screen keyboard — done and confirmed working well on real hardware.**
   Not on the original list, but not optional either: with no physical
   buttons and no keyboard paired, the device is otherwise mute at first
   boot. It injects HID keycodes through a plain callback using the exact
   wire format `enqueueKeyEvent()` already expects on the X4, so the editor,
   the dead-key handling and the BASIC layer need no changes to accept touch
   once that file is ported. Went through many real-hardware iterations
   (layout, spacing, Enter's shape, key coloring) before landing on something
   confirmed to work well — limited only by this panel's own touch-reading
   precision, not by the keyboard's design.

Things the PaperS3 makes newly possible, or newly necessary:

- **A real RTC.** The X4 had none, and file timestamps were reconstructed from
  a reader state file (with a known unfixed 3-hour offset). The BM8563 settles it.
- **Power-off is a pulse train**, not a level — `digitalWrite(LOW)` does not
  turn this board off. `freeink::m5papers3::powerOff()`.
- **`SCREEN 4` (graphics)** was never built on the X4 for want of RAM. A 1-bit
  960×540 framebuffer is 63KB against 8MB of PSRAM here.
- **Dual-boot/OTA** is M5Launcher's job on this device, so `OtaBootSwitch`
  most likely does not come across at all.

## License

MIT — see [LICENSE](LICENSE). Third-party font attribution in [NOTICE.md](NOTICE.md).
