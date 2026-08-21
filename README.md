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

**Milestones 1 and 2 — hardware bring-up + real SCREEN fonts — done and
confirmed on real hardware.** `editor/src/main.cpp` is a bring-up program,
not the firmware: it proves the EPD, the GT911 touch panel and the SD card,
and fills the entire 48×24 terminal grid with real `unscii_11x22` glyphs
(pangrams, digits, symbols, Portuguese accents) so legibility could be judged
on the physical panel rather than assumed.

The first attempt at this was garbled — traced to a real bug in
`emit_epdfont_header.py` (see "The font format changed underneath the port"
below), not a resolution limit. Fixed, verified by decoding the regenerated
header with the renderer's own bit-reading algorithm, and confirmed legible
on the device.

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

The GT911 `flipX`/`flipY` values (inherited from M5Paper v1.1 by analogy) are
**confirmed correct** via a real 4-corner-tap test on the physical device —
was listed unverified in the SDK's own profile, updated there too.

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
