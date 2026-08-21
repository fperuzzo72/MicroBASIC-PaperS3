# Third-party notices

## `editor/` — firmware base

Copied from [MicroWriter](https://github.com/fperuzzo72/MicroWriter)
(commit `97fb59c23bd09f895874885f9055eff19be21cc0`, branch
`microbasic-screen-editor-test`, 2026-08-17) — a **plain file copy**, not
a git fork/submodule and not kept in sync with upstream; MicroWriter's own
future changes aren't expected to be relevant to MicroBASIC's direction,
and anything that turns out useful later gets ported over deliberately
instead. `editor/LICENSE` (MIT, Joshua Hinton — MicroWriter's own editor
is itself imported from [MicroSlate](https://github.com/Josh-writes/microslate-firmware))
is preserved unchanged, as required by its license; MicroBASIC's own code
added on top (from `main.cpp`, `input_handler.cpp`, `ui_renderer.cpp`,
`screen_editor.{h,cpp}`, `config.h` onward) is this project's own MIT
license (see root `LICENSE`). Full detail on what's changed and why is in
`docs/DEVELOPMENT_LOG.md`.

`research/fonts/` carries the font source files behind the final
`SCREEN 0/1/2/3` font decision (see README.md and
`docs/DEVELOPMENT_LOG.md`) — **Unscii**, default, and **Terminus Bold**,
kept as a future settings-menu alternative, both at four sizes matching
32/48/64/80 columns. `previews/` holds only those 8 decided files
(480x800 1-bit BMPs, portrait-rotated to match how CPR-vCodex displays
them); `src/` carries the source files behind them plus every size/weight
tried and rejected along the way, kept for reference. Each family's own
license file is preserved in `research/fonts/src/`.

## Unscii — default font

`unscii-16.hex` (also `unscii-8.hex`, referenced but not used directly —
every Unscii preview derives from the 16-row glyphs), from
[viznut/unscii](https://github.com/viznut/unscii) by Ville-Matias
Heikkilä (viznut). Public domain / CC0 — full text in
`research/fonts/src/unscii-LICENSE`.

No native Unscii size reaches the project's 10x20px readability floor
(max native is 8x16), so all four `previews/` sizes are derived:
`Unscii_48col_16x32.bmp` and `Unscii_32col_24x48.bmp` are clean 2x/3x
nearest-neighbor upscales (exact integer ratios, no artifacts).
`Unscii_80col_10x20.bmp` and `Unscii_64col_12x24.bmp` are non-integer
ratios (1.25x/1.5x) and went through a multi-step algorithm fix — area-
coverage resampling, a 25% coverage threshold, a stem-width cap, and a
hand fix for the ç/Ç cedilla position — documented in full in
`docs/DEVELOPMENT_LOG.md` and in the module docstring of
`research/fonts/tools/generate_screen_fonts.py`, which reproduces all
four from `unscii-16.hex`.

## Terminus Font — future alternative

BDF files `ter-u20b.bdf`, `ter-u24b.bdf`, `ter-u32b.bdf` (bold; native
sizes cover 3 of the 4 `previews/` files directly) plus a 2x scale of
`ter-u24b.bdf` for the 4th (`TerminusBold_32col_24x48.bmp` — no native
Terminus Bold size reaches that large), from
[Terminus Font](https://terminus-font.sourceforge.net/) by Dimitar Zhekov
(mirrored at
[balabit-deps/balabit-os-8-xfonts-terminus](https://github.com/balabit-deps/balabit-os-8-xfonts-terminus)).
SIL Open Font License 1.1 — full text in
`research/fonts/src/terminus-LICENSE`.

The regular (non-bold) weight — `ter-u16n.bdf`, `ter-u20n.bdf`,
`ter-u24n.bdf`, `ter-u32n.bdf`, still in `src/` — read well too during
comparison but isn't part of the final two-font decision; kept for
reference.

## Rejected candidates (kept in `src/` for reference, not in `previews/`)

- **Spleen** (`spleen-8x16.bdf`, `spleen-12x24.bdf`, `spleen-16x32.bdf`,
  `spleen-32x64.bdf`) — BSD 2-Clause, from
  [fcambus/spleen](https://github.com/fcambus/spleen) by Frédéric Cambus.
  16x32 read well in testing but didn't make the final two-font cut.
  License: `research/fonts/src/spleen-LICENSE`.
- **Tamzen** regular and bold (`Tamzen6x12r.bdf`, `Tamzen8x16r.bdf`,
  `Tamzen10x20r.bdf`, `Tamzen10x20b.bdf`) — free to use/copy/modify, from
  [sunaku/tamzen-font](https://github.com/sunaku/tamzen-font), a fork of
  Scott Fial's [Tamsyn](http://www.fial.com/~scott/tamsyn-font/). Regular
  weight read too thin/light on e-ink; bold at 10x20 was legible "forcing
  it a bit" but lost the head-to-head against Terminus Bold at the same
  size. License: `research/fonts/src/tamzen-LICENSE`.
- **Ultimate Oldschool PC Font Pack** IBM CGA/EGA/VGA faces
  (`Px437_IBM_CGA.ttf`, `Px437_IBM_EGA_8x14.ttf`, `Px437_IBM_VGA_8x16.ttf`) —
  CC BY-SA 4.0, hardware-authentic recreations of real IBM adapter ROM
  fonts, by VileR, from
  [The Ultimate Oldschool PC Font Pack](https://int10h.org/oldschool-pc-fonts/)
  (mirrored at
  [retro-vault/font-vault](https://github.com/retro-vault/font-vault),
  [full CC BY-SA 4.0 text](https://creativecommons.org/licenses/by-sa/4.0/legalcode)) —
  any redistribution of these files, or images/fonts derived from them,
  must carry the same attribution and share-alike terms. Never made it
  into a narrowed-down round; not part of the final decision.

## My-Basic — the BASIC interpreter core

`editor/lib/MyBasic/my_basic.h` and `my_basic_src.inc` (renamed from
upstream's `my_basic.c` — see below), from
[paladin-t/my_basic](https://github.com/paladin-t/my_basic) by Tony Wang.
MIT — the full notice is preserved unchanged in each file's own header
comment (not duplicated here).

Two local modifications to `my_basic_src.inc`, both documented inline
and in `docs/DEVELOPMENT_LOG.md`: the internal (non-public-API) `_lock_t`
typedef is renamed to `_mb_lock_t` throughout, to resolve a collision
with ESP-IDF newlib's own `_lock_t`; and the file itself is renamed from
`my_basic.c` to `my_basic_src.inc` so PlatformIO's library dependency
finder doesn't compile it a second time on its own (it's `#include`d
once, deliberately, from `my_basic_impl.c`). Neither change is a
functional modification to the interpreter itself.

## MSX ROM font (HotBit) — never in `previews/`, never will be

A preview was rendered once, for personal comparison only, from the
character generator table of a `hotbit13p.rom` (32KB MSX1 BIOS+BASIC,
HB-8000-class Brazilian HotBit clone) dumped by the project owner from
their own hardware. Not part of the final font decision (its native cell
is a square 8x8, which doesn't fit the 2:1 width:height scaling approach
used for everything else here).

The **raw extracted font data** (the 2048-byte glyph table pulled out of
the ROM) has never been committed to this repo and won't be — the MSX
character ROM itself is copyrighted (Microsoft/ASCII Corporation
lineage), unlike every other font in this file. Kept local only, on the
project owner's own machine.
