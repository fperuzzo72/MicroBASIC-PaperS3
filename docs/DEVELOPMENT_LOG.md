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

`0x1105` is `ESP_ERR_NVS_NOT_ENOUGH_SPACE`. `editor/partitions.csv` reserves
only 20KB for `nvs` (`0x9000, 0x5000`) -- kept identical to CrossPoint's own
partition table on purpose, so their web flasher stays compatible (not
shared *code* like the areas above, but the same kind of borrowed-scheme
constraint: don't touch it). That one partition holds BLE bonds, saved WiFi
credentials, and now the WiFi radio's own calibration blob -- three growing
consumers sharing one small, fixed-size space. After a session's worth of
BLE pairing and WiFi credential testing, it filled: scanning (receive-only)
kept working regardless, but the calibration write specifically failed,
degrading the stack until actual association stopped completing at all --
worse on a second re-init within the same session
(`wifi_init_default: netstack cb reg failed with 12308`), which is why
retrying against a different network in the same session didn't help
either.

Fixed for now by erasing just the NVS region --
`esptool erase_region 0x9000 0x5000` -- which clears data *inside* the
existing partition table without touching the table itself, the bootloader,
or either app slot (same safety boundary as the OTA-slot-only flashing rule
elsewhere in this repo). That's a one-time reset, not a permanent fix:
nothing stops the same 20KB from filling up again over normal use, one
saved network or one new BLE pairing at a time. If it recurs, the real fix
is enlarging `nvs` in the partition table, which conflicts with the
match-CrossPoint constraint above and needs a real decision, not a quiet
change.

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
