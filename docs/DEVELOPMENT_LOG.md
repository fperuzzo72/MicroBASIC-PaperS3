# Development Log

## The X4 sibling and the shared surface

MicroWriter-BASIC-PaperS3 is a port of
[MicroBASIC](https://github.com/fperuzzo72/MicroBASIC), built for the
Xteink X4 (800×480 e-ink, no touch). Not a fork — a separate repository,
with its own history. The X4 is the full version: it started life as a copy
of the MicroWriter firmware and carries, alongside BASIC, the prose editor,
WiFi sync, a BLE keyboard, a file browser, and the VC picker.

Almost all of it is here now: BASIC, the screen editor, the prose editor, the
file browser, the BLE keyboard, and WiFi as file transfer. Two things stayed
behind on purpose. `ui_renderer.cpp` is the X4's button-driven UI, replaced
here by drawing written for touch, which was possible because `text_editor`
and `file_manager` turned out to have no dependency on it at all. And the
reading-progress sync inherited from MicroSlate is not something this project
wants.

The two repositories stay separate regardless of how much of the surface
ends up shared — one tree serving both a touch panel with an on-screen
keyboard and a button panel with a BLE keyboard would turn into conditional
logic on top of conditional logic.

The shared code, which grew as the port advanced — every row is a surface a
fix has to cross:

| Area | What it is |
|---|---|
| `research/fonts/tools/` | font pipeline: resizing, stem-width capping, header emission |
| `editor/src/screen_editor.*` | the character grid, line wrapping, logical lines |
| `editor/src/text_editor.*` | the prose buffer: cursor, selection, clipboard, one-level undo, pixel-budget wrap |
| `editor/src/file_manager.*` | the two collections, listing, titles, save with `.tmp`/`.bak` rotation |
| `editor/src/dead_keys.h`, `ascii_fold.h` | US-International composition, filename folding |
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

That erase was a one-time reset, not a permanent fix: nothing stopped the
same 16KB from filling up again over normal use, one saved network or one
new BLE pairing at a time.

**Permanently fixed 2026-08-24.** Once the device stopped needing
M5Launcher (see "The EPD rail was never powered" below), the
match-CrossPoint constraint that kept `nvs` at 16KB stopped applying —
their web flasher no longer has to be able to write this layout. The
partition table was rewritten with `nvs` at **32KB**, which is what the
device runs now and what `editor/partitions.csv` describes. Note the same
`erase_region 0x9000 0x5000` command *is* correct against this layout, by
coincidence rather than design — it now lands exactly inside the 32KB
partition.

## partitions.csv doesn't match the real device — RESOLVED

**Resolved 2026-08-24**: the device now runs the layout in
`editor/partitions.csv`, verified by reading `0x8000` back through
`gen_esp32part.py`. `platformio.ini`'s `maximum_size` and `offset_address`
were brought in step with it at the same time. Keep all three together if
the layout changes again. The rest of this entry is kept for the record,
since the drift it describes caused two separate misfires.

`editor/partitions.csv` (referenced by `platformio.ini`'s
`board_build.partitions`, and the source of the wrong 20KB assumption
above) described a partition table that had never actually been on this
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

## Two projects on one device: the shared dual-boot table

**2026-08-25.** The single-app table above was sized to MicroBASIC alone (a
3MB `app0`), which was enough for this firmware and nothing else. The
CrossPoint reader port builds to ~5.2MB and simply did not fit, so every
switch between the two projects would have meant rewriting the layout.

Replaced with one table, written to the device once, that both repos
describe identically:

```
nvs       data  nvs      0x9000     32K
otadata   data  ota      0x11000     8K
app0      app   ota_0    0x20000   6656K   <- CrossPoint
app1      app   ota_1    0x6A0000  6656K   <- MicroBASIC
coredump  data  coredump 0xD20000    64K
spiffs    data  spiffs   0xD30000  2880K   (reserved, unused)
```

Both slots are 6656K, not sized to today's binaries. CrossPoint's ~5.2MB sets
the number and MicroBASIC uses a quarter of its slot, which is the point: the
next project to land in a slot should not force a re-partition. The
`app0`/`test` subtype is gone -- both are real OTA slots now, so the
bootloader's own `otadata` selection picks between them and switching apps
rewrites 32 bytes rather than 1.7MB.

The drift this log already blamed twice is the thing to watch. There are now
*five* places holding the same numbers: this table, CrossPoint's
`partitions_m5papers3.csv`, and `board_upload.offset_address` /
`maximum_size` in each project's `platformio.ini`. The CSVs must stay
byte-identical below their comment headers. See `docs/DUAL_BOOT.md`.

Two traps found while wiring it up, both documented there:

* `pio run -t upload` writes four images, not one, and one of them is
  `boot_app0.bin` at the otadata offset -- which resets the selection to slot
  0. From this repo that means MicroBASIC flashes correctly into `app1` and
  then CrossPoint boots, which presents as "the flash didn't take".
* `esp_ota_get_next_update_partition()` means "the other project's slot" here,
  so CrossPoint's own SD/OTA self-update would have erased MicroBASIC. Guarded
  on that side by comparing the target's embedded `esp_app_desc_t.project_name`
  against the running app's, the same approach `patches/cpr-vcodex/`
  already takes against the CPR-vCodex fork.

**Both directions now work from the device** (confirmed on hardware, including
that a reboot returns to whichever app was last selected): the reader's Home
menu comes here, and the status bar's READER button goes back
(`editor/src/ota_apps.cpp`). `editor/boot-slot.sh` -- a wrapper over ESP-IDF's
`otatool.py`, so the otadata CRC comes from the vendor's implementation --
stays as the host-side fallback.

One deliberate divergence from CrossPoint's copy, which is otherwise kept
identical down to the identifier names so the two stay diffable: it writes the
new otadata entry as `ESP_OTA_IMG_NEW` and then rewrites it to `VALID` via
`confirmLastOtaSwitch()`, because its path is shared with a real self-update
whose rollback net `NEW` exists to arm. Nothing here shares that path -- this
only ever points at an already-flashed, previously-working sibling -- so
`ota_apps.cpp` writes `VALID` in the first place. Same end state, one flash
write instead of two. Left as `NEW`, the first reset before the app marks
itself valid (neither app calls `esp_ota_mark_app_valid_cancel_rollback()`)
rolls silently back to the other slot, which presents as waking from sleep in
the app you just left.

## A running clock is not a set clock

Files written by this firmware had no date at all: SdFat only fills a
directory entry's date fields when a callback is registered, and there was
none, so FAT read the zeros as 1980. The fix is a callback
(`editor/src/sd_datetime.cpp`) reading the board's RTC, with SNTP setting that
RTC the first time SYNC reaches a network.

The part worth remembering is what the first working version got wrong.
`Rtc::now()` returns false when the oscillator has stopped, so treating a
`true` as "the time is good" looks reasonable. It is not: the oscillator on
this unit was running, and reading back **2077-01-03**. That year is inside
the range FAT can represent, so every file would have been stamped with it,
and nothing downstream -- not the browser, not a computer opening the card --
could have told it was wrong. A plausible-looking wrong value is worse than an
obviously missing one, which is exactly why the old behaviour (1980, or the
1 Jan 2026 the card showed) was easier to notice than 2077 would have been.

Now the time is used only if it is one this firmware could be running at: not
before its own build date, since the firmware cannot predate itself, and not
decades ahead. When the RTC fails that test the log says so by name, rather
than silently falling back.

Same shape as the measurement bug below: the API answered a narrower question
than the one being asked of it. "Is the oscillator running" is not "is this
the time", just as "how wide is this glyph's ink" is not "how far does the
cursor move".

## Ink box is not advance: the same measurement bug, twice

`GfxRenderer::getTextWidth()` returns `maxX - minX` from
`EpdFont::getTextDimensions()` -- the **ink bounding box**, not the sum of
advances. For a whole sentence the difference is a few pixels of side bearing
and nothing notices. For a single glyph it is the whole side bearing, and for
a **space**, which has no ink at all, it is essentially zero.

That bit the prose editor twice, in two places that both looked like unrelated
layout bugs:

* **Word wrap.** `text_editor.cpp` asks the caller for a per-codepoint width
  and wraps against a pixel budget. Feeding it `getTextWidth()` of one
  character under-counted every character and counted spaces as free, so a
  line ran well past the panel before the budget was reached. On a prose line,
  where one character in six is a space, the error is large.
* **Caret and selection.** Positioning them by `getTextWidth()` of the prefix
  has the same flaw: a prefix ending in a space measures short, so the caret
  and the edges of the selection highlight land left of the text.

The fix for both is to measure the **advance**, which is
`width("cc") - width("c")`: the second copy starts exactly one advance further
along and contributes the same ink and bearings the first did, so everything
except the advance cancels. It gives the right answer for a space too. Cached
per codepoint, because the wrap pass re-measures the whole buffer on every
keystroke.

Wrapping and drawing now agree by construction: the wrap budget, the caret,
and the selection edges all sum the same advances that `drawText()` uses to
place glyphs. If a future screen needs to know where a character will land,
that is the measurement to use -- `getTextWidth()` answers a different
question, and answers it correctly.

## Two orderings that had to agree, written in separate places

`drawScreen()` decides which screen is on the panel, and `loop()` decides
which screen gets the keys. Both are if-else chains over the same conditions,
and they must list them in the same order or keys go to a screen that is not
the one being drawn. They were written months apart and disagreed: the WiFi
wizard came first when drawing, the file browser came first when routing.

Nothing noticed for as long as the two could not be open at once. MicroWriter
broke that assumption on its first run -- the browser is that machine's home
screen and is *always* active -- so the WiFi wizard drew correctly, took no
keystrokes at all, and could not be escaped from. It looked like "Esc is
broken in SYNC".

Fixed by matching the order, with a comment on the routing chain saying it
mirrors the drawing one. The general shape is worth remembering: when two
pieces of code must enumerate the same cases in the same order, and nothing
checks that they do, the bug waits for whichever future change first makes
both cases reachable together.

## A modal that bypasses processAllInput() must dirty the screen itself

Small, but the shape recurs. The READER confirmation routes keys in `loop()`
directly rather than through `processAllInput()`, deliberately, so nothing
types into the terminal behind the dialog. `processAllInput()` is also what
sets `screenDirty` on a keystroke, which is what schedules the repaint -- so
closing the dialog with Esc cleared the flag but left the dialog painted until
something else happened to dirty the buffer.

On e-ink that reads as "Esc did nothing", and it is worse than a cosmetic lag:
the keys after Esc go to the editor, behind a dialog that is visually still
there. The touch path never showed it, because `handleTouchTap()`'s caller
sets `screenDirty` whenever it returns true -- so the bug was invisible to
exactly the input method used to build the feature. The same gap also
swallowed the `"Switch failed"` message, which is printed after the last
repaint.

Both handlers now set `screenDirty` explicitly. Worth remembering when the
next modal is added: bypassing `processAllInput()` means taking over
everything it did, not just the routing.

## The EPD rail was never powered

**Symptom.** Booted on its own, without M5Stack's Launcher having run
first, the firmware came up completely normally -- SD mounted, WiFi
connected, BLE paired, full boot log -- and the panel never changed. Not a
crash, not a hang. Every draw call returned success with plausible timings
(`[GFX] Time = 29 ms from clearScreen to displayBuffer`, `[paint] returned
after 461 ms`). Only the glass disagreed.

**Root cause**, in `freeink-sdk`'s `LgfxEpdDriver.cpp`:
`FreeInkBusEPD::powerControl()` overrode LovyanGFX's
`lgfx::Bus_EPD::powerControl()` and never called the base implementation --
it ran the board's power hook and returned. That is right for LilyGo T5S3,
whose rails sit behind an I2C PMIC: there the hook *is* the power-up, and
the base class must not drive its `pin_pwr`/`pin_oe` (parked on a
placeholder GPIO). It is wrong for M5PaperS3, whose hooks are all `nullptr`
-- nothing replaced the base sequence, so this never ran:

```cpp
lgfx::gpio_hi(_config.pin_oe);    // GPIO45
lgfx::gpio_hi(_config.pin_pwr);   // GPIO46  <- the EPD rail
lgfx::gpio_hi(_config.pin_spv);   // GPIO17
```

`Bus_EPD::init()` configures all three as outputs, so they sat as outputs
driving LOW. The panel's electronics were simply never switched on. The SoC
doesn't care, which is why everything else worked and every diagnostic
lied. The panel is driven open-loop -- fixed waveform durations, no
ready/ack line to poll -- so nothing downstream can notice.

One line, in the end: delegate to the base class when the board supplied no
power hooks.

**Why it looked like a bootloader problem for so long.** The panel *did*
work once M5Stack's Launcher had run, because Launcher's own `M5GFX` drove
those same pins on its way past and they stayed up across the handoff. An
early A/B seemed to nail it: freshly-compiled bootloader -> dark panel,
Launcher's bootloader -> working panel. But both halves of that A/B ran the
same buggy `powerControl()`, and the real variable -- whether Launcher's
app had run at all -- moved together with the bootloader in every trial.
That single conflated experiment set the direction for a long time, and
several days of work inherited its premise.

**What the wrong premise cost, and what actually broke it.** Three things
were tried against the bootloader theory before the real cause surfaced,
each making the device worse rather than better: porting M5GFX's isolated
`gpio_lo(GPIO44)` line (harder hang, no serial output at all), porting its
full GT911-fingerprint-then-GPIO44 sequence (clean boot, panel still dark,
and a genuine brownout on a later run), and rewriting the partition table
to boot standalone (which did boot, and still didn't draw). Only the last
of those was a *clean* experiment -- one variable, this firmware as the
only app under the already-proven bootloader -- and its result is what
finally killed the bootloader theory outright, because the panel stayed
dark with the bootloader held constant.

The fix came from reading our own code against the vendor's rather than
running another trial: comparing `FreeInkBusEPD::powerControl()` side by
side with `lgfx::Bus_EPD::powerControl()` shows the missing three lines
immediately. That comparison was available from the first day.

**Lessons worth keeping.**

* An override that *replaces* a vendor base-class method, rather than
  wrapping it, is a defect waiting for the board whose config supplies
  nothing to replace it with. The hook design assumed every board would
  bring its own power topology; the one board that didn't got no power
  topology at all.
* `LgfxEpdConfig::pinPwr`'s own comment said the rail was "driven by
  LovyanGFX's own `Bus_EPD::powerControl`" -- the documented contract was
  correct and the code didn't honor it. When a comment and the behavior
  disagree, that gap is the bug, not a stale comment.
* This is the second silent failure in this same driver (see 7f549b7,
  which surfaced a discarded `init()` return for the same reason). An
  open-loop panel gives no feedback, so the driver has to be read, not
  observed.
* When a test changes two things at once, its result is worth about
  nothing, and it is much more expensive than a wasted afternoon: it
  points every later effort in one direction and stays unquestioned
  because it felt like evidence.

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

## The AltGr layer: written, verified, and deliberately not wired in

There is an `altgr_keys.h` in this project, next to `dead_keys.h`. **Nothing
calls it.** No `#include`, no call, not one line of code changed for it. It
sits there available and unused, on purpose, and this is the record of why.

### What it is

The other half of the US-International layout. The dead keys this project
already had cover the accents; the AltGr layer covers 37 keys that never
existed here: precomposed letters that need no dead key (AltGr+E gives e-acute
directly, AltGr+N gives n-tilde, AltGr+comma gives c-cedilla), letters with no
dead key route at all (eszett, a-ring, ae, o-slash, eth, thorn), currency,
inverted punctuation, maths, and the spacing accents on the apostrophe key.

`MOD_ALT_RIGHT` has been defined in `config.h` all along. It was simply never
read.

### Why it is not wired in

In practice there is no AltGr keyboard in use with this firmware, and the
virtual keyboard has no such key either. Wiring the layer would mean changing
code at every site that calls `deadKeyProcess()`, with nothing on the other end
to use it. The header stays, it costs nothing to keep, and the wiring can
happen the day there is a reason.

### How to wire it, if that day comes

The full walkthrough, with the code block for each site, is in
`US-International-IME-Android/docs/DEADKEY_TABLE_UPDATE.md`. In short:

1. `#include "altgr_keys.h"` beside `#include "dead_keys.h"`.
2. At every site that calls `deadKeyProcess()`, a branch **ahead of**
   `hidToAscii()`. It has to come first because AltGr characters are multi-byte
   UTF-8 and do not fit the `char` that `hidToAscii()` returns. Those sites
   already know how to insert a UTF-8 string, because the dead key engine
   already hands them one.
3. An AltGr character is a literal, so it resolves a pending accent in front of
   it. An empty position (AltGr+F) types nothing at all, as on Windows.

This was done once, in full, and then reverted: seven sites across three files:
`editor/src/terminal_input.cpp` (the screen editor and the program-key ring),
`editor/src/file_browser.cpp` (the text editor path), and all four X4 sites in
`editor/port-staging/input_handler.cpp`. Two of those live in translation units
that cannot see `capsLockOn`, which is a static inside
`editor/src/input_handler.cpp`, so wiring this needs an `inputCapsLockOn()`
accessor on `input_handler.h`. The program-key ring also stores **one byte per
key**, because BASIC strings are byte arrays, so the AltGr layer's three
non-Latin-1 characters, both curly quotes and the euro sign, would be dropped.
One place deliberately left alone even then: `handleTitleKey()` in
`file_browser.cpp` does not go through `deadKeyProcess()` at all, so it has no
dead key support to be consistent with.

### Two things that were left out alongside it

**Three missing dead key compositions.** Windows defines 56; this project has
53. Missing are `'y` giving y-acute, `'Y` giving Y-acute, and `"y` giving
y-diaeresis. Note the asymmetry on the last one: the Windows layout has the
lowercase and not the uppercase, so `"` then `Y` gives `"Y`, two characters.
That is the layout, not an oversight. The two-line patch is at
`US-International-IME-Android/docs/microslate-patch/dead_keys.patch`.

The rest of that audit is worth stating too: **of the 53 compositions the table
has, all 53 are correct.** Nothing wrong.

**Two dead keys in a row.** `deadKeyProcess()` resolves this through the
requeue, so `'` `'` `a` produces `'a-acute`. Measured on Windows: it gives
`''a`. Both dead keys come out as literals and the state is cleared. Measured
on macOS: `'a-acute`, matching this code. The two systems genuinely differ
here, because `KBDUSX.DLL` stores no dead+dead entries and the case falls to
the keyboard driver rather than the layout.

If fidelity to Windows is the goal, this is a behaviour to change, and the code
is in the document above. It was left out for the same reason as the rest:
there is no use for it right now.

### Where the specification came from

None of this was transcribed from memory. `KBDUSX.DLL`, the layout Windows
itself loads for "United States-International", is disassembled into
`US-International-IME-Android/tools/kbdusx.xml` (from kbdlayout.info), and
`altgr_keys.h` is **generated** from it by
`US-International-IME-Android/tools/gen_microslate_patch.py`.

The header was compiled under `-Wall -Wextra -Werror` and passes 15 lookup
assertions. And all 60 characters of the AltGr layer have glyphs in `EpdFont`:
checked against the intervals in `scripts/fontconvert.py`, where Latin-1
Supplement, General Punctuation and Currency Symbols are all already enabled.
There is nothing to wait for from the font on the day this gets wired in.

What never happened: none of it was compiled as firmware or tested on the
device.
