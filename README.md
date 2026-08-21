# MicroBASIC-PaperS3

A port of [MicroBASIC](https://github.com/fperuzzo72/MicroBASIC) — the "new"
1980s microcomputer that boots straight into a text-screen BASIC — from the
Xteink X4 to the **M5Stack PaperS3**.

The X4 is an ESP32-C3 with 380KB of RAM, a 1-bit 800×480 panel, and a 5-way
d-pad. The PaperS3 is an ESP32-S3 with 8MB of PSRAM, a 960×540 16-gray panel,
and **no physical navigation buttons at all**. The second of those is what
makes this a port rather than a new build target: every interaction has to be
rebuilt for touch.

## Status

**Milestone 1 — hardware bring-up.** `editor/src/main.cpp` is a bring-up
program, not the firmware: it proves the EPD, the GT911 touch panel and the SD
card on the real device, and draws the decided character grid at true size so
the cell dimensions can be judged with the eye before any font is regenerated
against them.

The X4 sources are carried in `editor/port-staging/` and move into
`editor/src/` one at a time as each is ported. Nothing there compiles for this
board yet — that is the point of staging them.

## Hardware layer

Hardware comes from [`freeink-sdk`](https://github.com/fperuzzo72/freeink-sdk)
(submodule, `m5papers3-support` branch), whose `M5PAPERS3` profile was
bench-tested on a physical unit — see `freeink-sdk/docs/m5papers3-support.md`
for the pin-by-pin CONFIRMED/PENDING breakdown. `GfxRenderer`, `EpdFont` and
`hal` are carried from
[crosspoint-reader-m5papers3](https://github.com/fperuzzo72/crosspoint-reader-m5papers3),
which already runs this panel; MicroBASIC's own copies were the older X4-era
versions with the panel size baked in as compile-time constants.

Two items in the SDK's profile are still listed as unverified and both are
exercised deliberately by the bring-up program: the GT911 `flipX`/`flipY`
values (inherited from M5Paper v1.1 by analogy, never confirmed on this chip
revision) and touch corner accuracy.

## Screen geometry — decided

Portrait, always. The terminal sits above a **fixed** on-screen keyboard,
which only works on the long axis.

```
   540 × 960 portrait
   ┌─────────────────┐
   │ 6│  48 × 24    │6│   terminal  528 × 528   cell 11 × 22
   │  │  cell 11×22 │ │
   ├─────────────────┤
   │    keyboard     │   540 × 432
   └─────────────────┘
```

48 columns does **not** divide 540 evenly (540/48 = 11.25). At an 11px cell,
48 columns is 528px, leaving a 6px margin each side. 24 rows at the matching
1:2 cell height (22px) is another 528px — so the terminal is an exact square,
which is about as close to a period CRT's proportions as this panel gets. The
keyboard takes the remaining 432px at full width; at this panel's ~234 ppi
that is roughly 5.9 × 9.3 mm per key, which is more generous than a phone's.

### SCREEN modes

| Mode | Columns × Rows | Cell | Scale from unscii-16 |
|---|---|---|---|
| `SCREEN 0` | 24×12 | 22×44 | 2.75× |
| `SCREEN 1` | 48×24 | 11×22 | 1.375× — **boots here** |

Only two, and that is a constraint rather than a choice. With the terminal
locked at 528×528 and cells kept at 1:2, the sizes that divide 528 evenly on
both axes are 11×22, 12×24 and 22×44 — and 44×22 (the 12×24 cell) is so close
to 48×24 that it earns nothing. Anything denser means an 8px-wide cell, which
is below the readability floor this project already established on hardware:

> Unscii's native Latin glyphs only come in 8x8 and 8x16 — both under this
> project's 10x20px "smallest still readable" floor.
>
> — `research/fonts/tools/generate_screen_fonts.py`

Both cells are non-integer rescales of unscii-16, which is the case
`AreaResampledHexFont` already exists to handle (the X4's 10×20 and 12×24 are
1.25× and 1.5×).

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
`advanceX = cell_w << 4`. The unconverted headers are parked in
`editor/port-staging/_fonts_x4_unconverted/` rather than in the font directory,
so they cannot be included by accident.

The trailing `EpdFontData` fields added since (groups, kerning classes,
ligatures, glyph-miss handlers) are all safe to leave out: the headers use
positional aggregate initialization, so the new members value-initialize to
zero/`nullptr`, which is exactly "no compression, no kerning, no ligatures".
`ascender` must still be 0 to match `top=0` — `drawText()` still adds it.

## Flashing

**Do not `pio run -t upload`.** `board_upload.offset_address` is `0x10000`,
which on this device is where M5Launcher itself lives:

```
app0    test   0x10000   1536K   <- M5Launcher
crossp  ota_0  0x1a0000  5312K
paperb  ota_1  0x6d0000   512K
factor  ota_2  0x750000  1344K
dos000  ota_3  0x8a0000  1792K
```

Build the binary and let M5Launcher install it from the SD card root.

## What has to work

1. Text editor with the US-International keyboard layout and dead keys.
   TypeWriter and Clean screen modes are dropped; the standard mode only.
2. BASIC interpreter and screen editor.
3. File read/write, creating new `.txt` and `.bas` files.
4. WiFi and the file-transfer web server.
5. Bluetooth keyboard.
6. **On-screen keyboard.** Not on the original list, but not optional either:
   with no physical buttons and no keyboard paired, the device is otherwise
   mute at first boot. It injects HID keycodes into `enqueueKeyEvent()` —
   the single funnel every key already passes through — so the editor, the
   dead-key handling and the BASIC layer need no changes to accept touch.

Things the PaperS3 makes newly possible, or newly necessary:

- **A real RTC.** The X4 had none, and file timestamps were reconstructed from
  a reader state file (with a known unfixed 3-hour offset). The BM8563 settles it.
- **Power-off is a pulse train**, not a level — `digitalWrite(LOW)` does not
  turn this board off. `freeink::m5papers3::powerOff()`.
- **`SCREEN 4` (graphics)** was never built on the X4 for want of RAM. A 1-bit
  540×960 framebuffer is 63KB against 8MB of PSRAM here.
- **Dual-boot/OTA** is M5Launcher's job on this device, so `OtaBootSwitch`
  most likely does not come across at all.

## License

MIT — see [LICENSE](LICENSE). Third-party font attribution in [NOTICE.md](NOTICE.md).
