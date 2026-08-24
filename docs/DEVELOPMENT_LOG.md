# Development Log

## The X4 sibling and the shared surface

MicroBASIC-PaperS3 is a port of
[MicroBASIC](https://github.com/fperuzzo72/MicroBASIC), built for the
Xteink X4 (800×480 e-ink, no touch). Not a fork — a separate repository,
with its own history. The X4 is the full version: it started life as a copy
of the MicroWriter firmware and carries, alongside BASIC, the prose editor,
WiFi sync, a BLE keyboard, a file browser, and the VC picker.

This port doesn't have those parts yet — that's because it's a port in
progress, not because they're out of scope by design. They're expected to
land here eventually (the on-screen keyboard already has a working BLE
keyboard alongside it, ported ahead of the rest). That means the shared
surface below is going to grow, not stay stable, and the habit of carrying
a fix to both sides matters more over time, not less: `text_editor`,
`file_manager`, and `ui_renderer` will all join it as the port advances,
each one adding another surface a fix has to cross.

The two repositories stay separate regardless of how much of the surface
ends up shared — one tree serving both a touch panel with an on-screen
keyboard and a button panel with a BLE keyboard would turn into conditional
logic on top of conditional logic. When the text editor itself gets ported,
it'll bring undo, `.bak` discard, and `ascii_fold` along with it — already
done and confirmed on X4 hardware.

The four areas that are shared code today:

| Area | What it is |
|---|---|
| `research/fonts/tools/` | font pipeline: resizing, stem-width capping, header emission |
| `editor/src/screen_editor.*` | the character grid, line wrapping, logical lines |
| `editor/src/tb_*` | interpreter integration and the runtime contract |
| `patches/tinybasic/` | the patch set over upstream |

There's no automation enforcing this, and at this scale a submodule would
cost more friction than it would save — so it's a manual habit, written
down because a habit that isn't written down isn't a habit. **If you touch
any of the areas above, carry the change to the other repo too.**

## Four bugs found here, latent on the X4 for months

None of this is theoretical — this port hit all four of the following in a
single session, each because a different panel size exercises an assumption
the X4's own 800×480 dimensions never question: glyph widths that aren't
multiples of 8, a logical line wrapping into a different number of physical
rows. Porting turned out to be a test suite nobody would have thought to
write.

### 1. Glyph bit-packing

The font emitter packed each bitmap row into its own byte-aligned bytes,
while the renderer reads the glyph as one continuous bitstream with no
padding between rows. The two schemes are byte-identical for any cell width
that happens to be a multiple of 8 — exactly why it went unnoticed on the
X4 (SCREEN 0/1, widths 24/16) while SCREEN 2/3 (widths 12/10) were quietly
garbled from each glyph's second row onward. Fixed via
`pack_bits_contiguous()` in `research/fonts/tools/emit_epdfont_header.py`.
See the README's "The font format changed underneath the port" for the full
account on this repo's side.

### 2. The stem-width cap flattening `-` and `_`

`cap_stem_width()` caps a row's run down to `MAX_STEM_WIDTH` when that run
is close to the glyph's own typical (median) run width — meant to catch a
vertical stem that got fattened by the resize's upscale. It never
distinguished that case from a glyph that's legitimately just one wide
horizontal bar: for `-`, the bar's own length *is* the glyph's typical run
width (there's nothing else in the glyph to average against), so it always
read as "a stem sitting right at the cap" and got chopped to 3px regardless
of how wide it was supposed to be. Same for `_` and `=`.

Fixed by only capping a run when it's part of a feature that spans at least
6 rows vertically: real stems (`I`, `l`, `T`, `1`) span 12-22 rows in this
project's fonts; a lone horizontal bar spans 2-3. See
`research/fonts/tools/generate_screen_fonts.py`'s `cap_stem_width()`.

### 3. Backspace orphaning text on a continuation row

### 4. The cursor landing inside the line it had just registered

## Notes on 3 and 4 — where this port's fix diverged from the X4's

**3.** The fix applied here (`editor/src/screen_editor.cpp`,
`screenEditorBackspace()`) was, in an earlier pass, to remove the line that
unconditionally cleared `rowIsContinuation[cursorRow]` when backspacing
across a wrap boundary. That fixes the reported case — navigating onto a
wrapped row and backspacing across the boundary no longer silently drops
text still sitting on that row — but introduces its mirror: a tail row
that's been backspaced down to empty stays marked as a continuation
forever, and a fresh line typed there afterward gets glued to the row above
on the next Enter.

On the X4 this landed with the missing piece included from the start:

```cpp
bool rowEmpty = true;
for (int c = 0; c < cols; c++)
  if (grid[cursorRow][c] != (uint32_t)' ') { rowEmpty = false; break; }
if (rowEmpty) rowIsContinuation[cursorRow] = false;
```

The line existed for a legitimate reason — unwrapping a row that's
genuinely done being a continuation. What was missing was the condition:
unwrapping is exactly the case where the tail row has nothing left in it.
This repo's `screenEditorBackspace()` now carries the same conditional
clear.

**4.** Only shows up once 3 is fixed — before that, the vanishing text hid
the cursor problem behind it. The fix, in `screenEditorStartNewInputLine()`
on both sides:

```cpp
cursorRow = logicalLineEndRow();   // before advancing
```

Descend from the end of the *logical* line the cursor is within, not the
physical row it happens to be sitting on. Reuses the function that already
walks the continuation chain both ways, so it holds for a two-row wrap or a
five-row one without any extra casework.

## WiFi never actually connected: NVS too small for PHY calibration

Every `WiFi.begin()` reached the driver correctly (`[wifi] connecting to
...` logged fine) but never got past `WL_DISCONNECTED`, across every network
tried -- the home AP and, separately, a phone hotspot -- with the password
confirmed correct via a temporary serial dump of what was actually typed.
It looked at first like the ESP32-S3 WiFi/BLE coexistence errata already
worked around in `wifi_sync.cpp` (`suspendBleForWifiConnect()` /
`resumeBleAfterWifiConnect()`, still worth keeping -- see that comment), but
it kept failing even in a test where BLE had never been started at all,
which ruled that out as the primary cause.

The real error was sitting at boot, on the very first WiFi scan of the
session:

```
E (31855) phy_init: store_cal_data_to_nvs_handle: store calibration data failed(0x1105)
```

`0x1105` is `ESP_ERR_NVS_NOT_ENOUGH_SPACE`. The device's real `nvs`
partition (confirmed by reading the live table off the device itself --
see "partitions.csv doesn't match the real device" below, this repo's own
`editor/partitions.csv` is *not* the ground truth here) is only 16KB
(`0x9000`, 16K), kept small on purpose to match CrossPoint's own partition
table so their web flasher stays compatible. That one partition holds BLE
bonds, saved WiFi credentials, and now the WiFi radio's own calibration
blob -- three growing consumers sharing one small, fixed-size space. After
a session's worth of BLE pairing and WiFi credential testing, it filled:
scanning (receive-only) kept working regardless, but the calibration write
specifically failed, degrading the stack until actual association stopped
completing at all -- worse on a second re-init within the same session
(`wifi_init_default: netstack cb reg failed with 12308`), which is why
retrying against a different network in the same session didn't help
either.

Fixed for now by erasing the NVS region -- `esptool erase_region 0x9000
0x5000` -- reasoning at the time that this stayed *inside* the existing
partition table without touching the table itself, the bootloader, or
either app slot. That reasoning used `editor/partitions.csv`'s declared
20KB `nvs` size, which turned out to be wrong: the real partition is only
16KB, so this command actually over-ran the real `nvs` partition by 4KB,
into the first of `otadata`'s two 4KB sectors (see the entry below -- ESP-IDF
keeps `otadata` as two redundant sectors specifically so one going bad
doesn't strand the device, which is almost certainly why this had no visible
effect: the device kept booting into `crossp` normally on every test since).
Erasing was still the right instinct, just aimed with the wrong map -- next
time, read the live table first rather than trusting `partitions.csv`.

That erase is a one-time reset, not a permanent fix: nothing stops the same
16KB from filling up again over normal use, one saved network or one new
BLE pairing at a time. If it recurs, the real fix is enlarging `nvs` in the
partition table, which conflicts with the match-CrossPoint constraint above
and needs a real decision, not a quiet change.

## partitions.csv doesn't match the real device

`editor/partitions.csv` (referenced by `platformio.ini`'s
`board_build.partitions`, and the source of the wrong 20KB assumption
above) describes a partition table that has never actually been on this
device: 20KB `nvs` at the right offset but the wrong size, `otadata` at a
different offset, `app0`/`app1` at 6400KB apiece with an `spiffs` partition
neither the real table nor this project uses, no separate `phy_init`
partition at all. Reading the live table directly off the device (`esptool
read_flash 0x8000 0xC00` + `gen_esp32part.py`, both from the README's
"Flashing" section) on 2026-08-24 confirmed it instead matches the table
the README already documented from 2026-08-21, unchanged -- exactly what
you'd expect, since this project's hard rule is to never flash the
partition table itself.

The one place this silently mattered before now: `platformio.ini`'s
`board_upload.maximum_size` was set to `6553600` (6400KB), matching
`partitions.csv`'s `app0`/`app1` size -- not the real `crossp` slot's actual
5312KB. A build under 5312KB (everything built so far, currently ~1.76MB)
was never at risk, but PlatformIO's own "Checking size" step would have
happily approved a firmware between 5312KB and 6400KB as fitting, when
flashing it into the real, smaller `crossp` slot at `0x1a0000` would have
written past that slot's actual boundary. Fixed by setting `maximum_size`
to `5439488` (5312KB) to match the real slot. `partitions.csv` itself is
left as-is -- correcting its *contents* to describe the real table would
mean generating and flashing a matching partition-table image, which is
exactly the operation this project's hard safety rule forbids; it only
turned out to matter here because one number derived from it (the size
ceiling) leaked into a place that does matter, and that number is now
fixed at its source instead.

## EPD stays dark without Launcher

This device's firmware only draws anything when M5Stack's own Launcher
(the picker menu at `app0`) runs first. Flashed and booted entirely on its
own -- same bootloader, same app code either way -- the app runs completely
normally: SD card mounts, WiFi connects, BLE pairs, the whole serial boot
log looks identical to a working boot. The EPD panel itself just never
updates, staying on whatever was on screen before (or blank, on a truly
cold boot). Not a crash, not a hang -- the firmware is alive and well; the
panel alone doesn't respond.

**First hypothesis, ruled out: the bootloader itself.** The original
(pre-this-investigation) theory was that a freshly-compiled bootloader was
somehow different from Launcher's own -- confirmed once, early on, via A/B:
same app, panel dead under a from-scratch bootloader, working under
Launcher's. That test conflated two variables at once, though: a different
bootloader *and* a different first app to run (since replacing the
bootloader also meant Launcher's own app never ran). This session finally
separated them with a controlled test: this project's own known-working
firmware, written as the *only* app on the device (a from-scratch 3-partition
table -- NVS, otadata, one 3MB `app0`, coredump -- built and verified with
`gen_esp32part.py`, flashed under the *same* bootloader already proven to
work), still came up with a dark panel on boot, log identical to every other
"works over serial, panel stays dark" case. The bootloader is not the
variable. (Getting to this clean test took two false starts: once, the
firmware image was simply too large for the undersized real `app0` partition
--1,762,768 bytes into a 1,572,864-byte slot -- which the bootloader
correctly rejected outright rather than silently overflowing, an
inconclusive, self-inflicted result, not a hardware finding.)

**Second hypothesis, confirmed: M5Stack's own `M5GFX`/`M5Unified` libraries
do something at boot that this project's own from-scratch EPD driver
doesn't.** Every third-party PaperS3 project found that draws successfully
without going through M5Stack's own Launcher or UIFlow2 first goes through
`M5GFX`/`M5Unified` directly (confirmed for
[bmorcelli/Launcher](https://github.com/bmorcelli/Launcher) itself,
`gebeto/microreader-paper-s3`, and a Sudoku gist by `palaniraja`) -- none of
them reimplement the EPD bus from scratch the way this SDK does. Reading
`M5GFX.cpp`'s own `board_M5PaperS3` autodetect code found the likely
mechanism: right after identifying the board and before constructing
`Bus_EPD`/`Panel_EPD`, it electrically fingerprints GPIO41/42 (the touch
I²C pins: drive both low, then high, then read back as inputs with a
pulldown -- both must still read HIGH, proof an external pull-up survives
it), then probes I²C for a GT911 answering at `0x14` or `0x5D`, and only if
that succeeds does it assert GPIO44 (`PWROFF_PULSE_PIN`) LOW. This project's
own `BoardConfig.h` had previously assumed (marked PENDING) that no
boot-time hold/enable pin was needed here, reasoning the board "appears to
self-latch through its own power-button circuit" -- true for staying
powered on, apparently not true for the EPD rail specifically.

**Two attempts at porting this into `freeink::m5papers3::prepare()`
(wired into `LgfxEpdConfig::power.prepare`, called by `LgfxEpdDriver`
before the EPD bus is touched at all) did not fix it:**

1. Porting *only* the isolated `pinMode(44, output); gpio_lo(44);` line
   made things **worse** -- a harder, earlier hang with no boot log at all,
   not even the otherwise-normal "runs fine, panel dark" boot this SDK
   already reproduced.
2. Porting the *full* sequence (fingerprint, then a real GT911 probe over
   `Wire`, then the GPIO44 write gated on finding it) fared differently
   depending on context: under the *new* single-partition table (no
   Launcher involved at all), it completed a clean full boot including a
   successful-looking `displayBuffer()` call -- but the panel still didn't
   show the correct content, later runs of the same build produced a real
   brownout at boot, and a version with added `Serial.printf` diagnostics
   around each stage hung *before Serial output started at all*, hard
   enough that the only recovery was holding the device into its ROM
   download-mode strap and reflashing from there (a physical reset, and
   even a normal power-cycle, did not bring the USB serial port back).

Both attempts were reverted; the device is back on the original 6-partition
table with Launcher's own bootloader and app0, and this project's firmware
in `crossp`, the only currently-confirmed-working arrangement. The exact
missing piece is still open -- plausibly something in `Panel_EPD`/`Bus_EPD`'s
own internal state that depends on timing or ordering not yet matched, or a
step elsewhere in `M5GFX::begin()` not yet identified. Whoever picks this up
next: instrument before guessing again -- the two attempts above show this
hardware punishes an incomplete port of this sequence more than having none
of it at all, and the failure modes get *worse*, not better, between
attempts, which is a warning sign, not noise to iterate past casually.

## Unconfirmed lead: possible tokenizer string-literal corruption

Seen once, not yet reproduced: a program typed as

```
10 PRINT "Teste de salvamento de programa"
20 PRINT "Deu certo!"
```

came back from `LOAD` (after `SAVE`) with garbage appended to line 20 --
specifically a fragment of line 10's own string, **uppercased and with
spaces removed**: `20 PRINT "Deu certo!"LVAMENTODEPROGRAMA""`.

`LOAD` only reads back what `SAVE` wrote, and `SAVE` only writes back
whatever was already in the interpreter's program memory from the moment
Enter was pressed on line 20 -- so this isn't a file I/O bug, and the
"uppercase, no spaces" shape doesn't match anything the screen editor does
(it never touches letter case). It *does* match `basic.c`'s own
identifier/keyword lexer (around line 3332, `basic.c`): it scans a run of
`[a-zA-Z_@]` characters and uppercases each one **in place in the input
buffer** as it goes, character by character -- which is exactly "letters
survive, case flips, whitespace breaks the run" behavior. That lexer is
only supposed to run *outside* string literals; if its quote-tracking ever
loses sync (treating the tail of an unrelated earlier string as if it were
outside quotes on a later pass), the content it mangles would look exactly
like this.

Not reproduced since (a clean retype-and-resave loaded correctly), and
too deep in the vendored upstream tokenizer to chase blind without a live
repro. If it recurs: `LIST` *before* `SAVE` on the affected line, plus the
exact edit history that produced it, is what turns this into something
fixable. Worth checking for on the X4 side too if this ever gets confirmed
here -- both projects vendor the same upstream `basic.c` under their own
`patches/tinybasic/`, so a real tokenizer bug would very likely reproduce
on the X4 as well, not just this port.
