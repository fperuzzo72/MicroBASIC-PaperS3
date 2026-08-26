# MicroBASIC-PaperS3

A port of [MicroBASIC](https://github.com/fperuzzo72/MicroBASIC) — the "new"
1980s microcomputer that boots straight into a text-screen BASIC — from the
Xteink X4 to the **M5Stack PaperS3**.

The X4 is an ESP32-C3 with 380KB of RAM, a 1-bit 800×480 panel, and a 5-way
d-pad. The PaperS3 is an ESP32-S3 with 8MB of PSRAM, a 960×540 16-gray panel,
and **no physical navigation buttons at all**. The second of those is what
makes this a port rather than a new build target: every interaction has to be
rebuilt for touch.

**Runs standalone — no launcher, no dependencies.** This was not true until
2026-08-24: the panel only drew if M5Stack's own Launcher had run first,
which looked for a long time like this port needing Launcher's bootloader.
It turned out to be a one-line bug in this project's own EPD driver, which
never drove the panel's power rail and quietly relied on Launcher having
left those pins high. See `docs/DEVELOPMENT_LOG.md`'s "The EPD rail was
never powered" — the failure mode is worth knowing about, because every
diagnostic said "success" the whole time.

## How it works

### Commands

Three commands are this project's own, intercepted before the line ever
reaches the interpreter (`editor/src/input_handler.cpp`):

| Command | Does |
|---|---|
| `SCREEN` | prints the current mode (0-3) |
| `SCREEN <n>` | switches to that mode (see the table below) |
| `FILES`, `DIR` | aliases for `CATALOG` — list `/MicroBASIC/programs` |
| `SYNC` | opens the WiFi file-transfer wizard — same as tapping the "SYNC" status button |

Everything else — `PRINT`, `LET`, `INPUT`, `IF`/`THEN`, `FOR`/`NEXT`,
`GOSUB`/`RETURN`, `DIM`, `READ`/`DATA`, `LOAD`, `SAVE`, `CATALOG`, `DELETE`,
`RUN`, `LIST`, `NEW`, `CLS`, `LOCATE`, `GET`, and the rest of the language —
is Stefan Lenz's TinyBASIC (vendored in `editor/lib/TinyBasic/basic.c`,
patched in `patches/tinybasic/`). For the full language reference, see
[slviajero/tinybasic](https://github.com/slviajero/tinybasic). `LOAD`/`SAVE`/
`CATALOG`/`DELETE` all operate on `/MicroBASIC/programs` on the SD card,
created automatically at boot if missing.

### Keyboard

Two independent input paths, both feeding the same key queue — the editor,
dead-key handling, and BASIC's `GET` don't know or care which one a
keystroke came from.

**On-screen.** Always available; the "KBD" status button (top right)
shows/hides it. Full US-International layout with dead-key composition.
Forced visible for the whole WiFi wizard (see SYNC below), even if a BLE
keyboard is connected, so losing BLE mid-flow never strands the wizard with
no way to type.

**BLE.** Starts only when the "BLE" status button is tapped — not
automatically at boot. (This is deliberate, not an oversight: starting BLE
unconditionally at boot meant it was also live during every WiFi connect
attempt, which is its own problem — see below.) Once started:

- Auto-pairs with the first HID-advertising keyboard it finds (no pairing
  picker UI exists yet — one device at a time).
- Auto-reconnects to that saved bond for the rest of the session.
- If reconnecting doesn't succeed for **20 seconds**, falls back to scanning
  for any HID-advertising device again — including a *different* keyboard,
  not just the same one.
- Tapping "BLE" again jumps straight to a fresh scan immediately, skipping
  that 20-second wait — the way to deliberately pair a different keyboard
  without powering off the old one first.
- The "BLE" status button reflects live connection state (filled = connected,
  outlined = not) so this is visible at a glance, not just inferred from
  whether typing works.
- Passkey-pairing keyboards (not Just Works) show their 6-digit code in the
  terminal — the interpreter always generates `123456`, so it's really just
  a confirmation prompt, not a real per-device code.

**BLE and WiFi can't both be radio-active at the same time** (a documented
ESP32-S3 coexistence limitation, not a bug in either): connecting to WiFi
suspends BLE entirely for the ~1-25 seconds the connection attempt takes,
then resumes it automatically once that concludes (success, timeout, or
cancel). The keyboard reconnects on its own; nothing needs to be re-paired.

### SYNC — WiFi file transfer

Reached by tapping the "SYNC" status button or typing `SYNC`. No
sync/reading-progress feature (that's the X4/MicroSlate-inherited part this
port deliberately doesn't carry over) — just a network picker, a password
entry, and a small HTTP file server.

1. **Scanning** — lists nearby networks, `[saved]` marks ones with a stored
   password, `(locked)` marks ones that need one.
2. **Network list** — Up/Down (physical, BLE, or on-screen keyboard) moves
   an inverted highlight bar; Enter selects.
3. If the network has a saved password, it **auto-connects** immediately.
   Otherwise, **password entry** — typed characters show as `*`.
4. **Connecting** — up to **25 seconds**. Esc cancels at any point.
5. On success, a **new** password gets a yes/no save prompt; a **saved**
   password that's since stopped working (after a 25s timeout) gets a
   yes/no forget prompt instead.
6. **Syncing** — the status line shows a URL:
   `http://<device-ip>/` (the mDNS name `microbasic-papers3.local` may also
   work, depending on the network/OS). Open that on a computer on the same
   network: a drag-and-drop page for uploading/downloading `.bas` programs
   against `/MicroBASIC/programs`.
7. Esc at any point (or leaving the SYNC screen) tears the WiFi connection
   and the HTTP server down cleanly.

Credentials are saved in NVS (and mirrored to `/MicroBASIC/wifi.json` on the
SD card) — see `docs/DEVELOPMENT_LOG.md`'s "WiFi never actually connected"
entry for a real bug this hit (an undersized NVS partition silently blocking
the WiFi radio's own calibration) if WiFi ever stops connecting again after
heavy BLE-pairing/WiFi-credential testing in one session.

### SCREEN modes

See the table under "Screen geometry" below for the four resolutions and
their column/row counts.

## Sibling project

MicroBASIC is not a fork of this repo, and this isn't a fork of it either —
two separate repositories, each with its own history. The X4 is the full
version: it started life as a copy of the MicroWriter firmware and carries,
on top of BASIC, the prose editor, WiFi sync, a BLE keyboard, a file
browser, and the VC picker.

Two of those four are already here: **BLE keyboard** and **WiFi** (as file
transfer, not the X4's own reading-progress sync — see "How it works"
above). The prose editor and the file browser are still missing, and that's
because this is a port in progress, not because they're out of scope by
design — they're expected to land here eventually. That means the shared
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

## Status — v0.4

This is a working computer now, not a bring-up demo: BASIC with a full
screen editor, LOAD/SAVE/CATALOG against the SD card, four SCREEN text
resolutions, a BLE keyboard and an on-screen one, and WiFi file transfer —
see "How it works" below for the full reference. `editor/src/main.cpp` is
the actual firmware's entry point.

The panel, touch, and SD card bring-up that v0.1-0.3 proved out are still
in there underneath: real glyphs on the physical panel rather than assumed
legible, and a full touch keyboard (`osk.cpp`/`osk.h`) doing US-International
dead-key composition.

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
— see "How it works" above for how both actually behave.

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
default here. All four fonts are generated, verified, and switchable at
runtime with the `SCREEN <mode>` command (see "How it works" below).

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

The dev unit carries **two firmwares at once** — MicroBASIC and the
CrossPoint reader port — so both can be developed against the same physical
board without reflashing the layout every time. The table in
`editor/partitions.csv` is a device-wide contract shared with
`crosspoint-reader-m5papers3`, not this project's own file; see
[docs/DUAL_BOOT.md](docs/DUAL_BOOT.md) before changing it.

```
nvs       data  nvs      0x9000     32K
otadata   data  ota      0x11000     8K
app0      app   ota_0    0x20000   6656K  <- CrossPoint (~5.2MB used)
app1      app   ota_1    0x6A0000  6656K  <- MicroBASIC (~1.7MB used)
coredump  data  coredump 0xD20000    64K
spiffs    data  spiffs   0xD30000  2880K  (reserved; unused)
```

Build the app, then write *just* `firmware.bin` into MicroBASIC's slot:

```bash
esptool.py --chip esp32s3 --port <port> --baud 921600 \
    write_flash 0x6A0000 .pio/build/m5papers3/firmware.bin
```

Switch which app boots with `editor/boot-slot.sh 0` (CrossPoint) or
`editor/boot-slot.sh 1` (MicroBASIC) — that only rewrites `otadata`, never an
app image. **Never `pio run -t upload`**: it also writes the bootloader, the
partition table, and a `boot_app0.bin` that resets `otadata` to slot 0, so the
flash lands correctly and then the *other* app boots.

Read the live table first if in any doubt (`esptool read_flash 0x8000
0xC00` piped through `gen_esp32part.py`) — offsets move whenever the layout
is changed, and this one has been changed.

**Still worth not writing `0x0` casually.** The bootloader currently on the
device is M5Launcher's original, never overwritten. It is no longer
*believed* to be load-bearing — the "only Launcher's bootloader works"
theory was disproven, see the note at the top of this README — but a
freshly-compiled bootloader hasn't been tested since the real bug was
fixed, so "probably fine" is the honest status, not "verified". If you do
replace it and the panel goes dark, that's the thing to undo first.

Full-flash backups of the dev unit (bootloader + partition table + every
app, restorable in one `write_flash 0x0 <backup>.bin`) live outside this
repo at `~/Desktop/M5PaperS3-backup/`, with their own `RESTAURAR.md`. Three
of them, including one taken with M5Launcher + CrossPoint intact, which is
what to restore if the Launcher menu is ever wanted back.

## What has to work

1. **BASIC interpreter and screen editor — done and confirmed on hardware.**
   TinyBASIC plus the same character-grid screen editor as the X4 (logical
   lines, wrapping, LIST/RUN/etc.) — see "How it works" above.
2. **File read/write — done and confirmed on hardware.** `LOAD`/`SAVE`/
   `CATALOG`/`DELETE` (TinyBASIC's own) against `/MicroBASIC/programs`, plus
   this project's own `FILES`/`DIR` aliases. `.bas` only for now — no `.txt`,
   since there's no text editor yet (see item 4).
3. **WiFi and the file-transfer web server — done and confirmed on
   hardware.** See "How it works" above.
4. Text editor with the US-International keyboard layout and dead keys.
   TypeWriter and Clean screen modes are dropped; the standard mode only.
   **Dead-key composition itself is done and confirmed** — `dead_keys.h`
   (self-contained, no editor/BLE/wifi dependencies) is ported and wired into
   the terminal's typing path already; the text editor it belongs in
   (`text_editor.cpp`) is still in `port-staging/`. Next up for v0.5.
5. **Bluetooth keyboard — done and confirmed on hardware.** See "How it
   works" above.
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
- **Dual-boot/OTA** was M5Launcher's job on this device. There is no picker
  in front of the apps any more, but the device does hold both MicroBASIC and
  the CrossPoint reader, selected by `otadata`
  ([docs/DUAL_BOOT.md](docs/DUAL_BOOT.md)) — today over USB, from the host.
  An on-device MENU entry that switches to the reader and reboots would mean
  bringing `OtaBootSwitch` across after all.

## License

MIT — see [LICENSE](LICENSE). Third-party font attribution in [NOTICE.md](NOTICE.md).
