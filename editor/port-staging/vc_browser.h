#pragma once

#include <cstdint>

// VC -- a full-screen program picker in the spirit of Volkov Commander:
// a multi-column list of the programs directory with a highlighted
// selection you move with the arrow keys, an inverse-video title bar and a
// key-hint bar at the bottom.
//
// Deliberately *not* drawn into the screen editor's character grid: it
// renders straight to the panel from its own state (see drawVcBrowser in
// ui_renderer.cpp), so whatever was on the terminal is still there when you
// leave. It does derive its whole layout from the active SCREEN mode's
// metrics, so it reflows automatically across SCREEN 0-3 -- one column of
// names on SCREEN 0's 32x10, three across SCREEN 3's 80x24.
//
// No box-drawing glyphs are used: the fonts here cover ASCII plus Latin-1
// only (checked -- the generated headers' intervals are 0x20-0x7E and
// 0xA0-0xFF), so the framing is done with inverse-video bars instead, which
// looks closer to the real thing than an ASCII +---+ frame would anyway.

void vcOpen();   // rescans the programs directory and resets the selection
void vcClose();

// Returns true if the key was consumed. Enter loads the selected program
// and closes; Escape closes without loading.
void vcHandleKey(uint8_t keyCode, uint8_t modifiers);

// --- Read-only view, for the renderer ---
int vcFileCount();
const char* vcFileName(int index);        // "" when out of range
unsigned long vcFileSize(int index);
int vcSelectedIndex();
int vcScrollTop();                        // index drawn at the top-left cell

// Layout, all derived from the current SCREEN mode. Shared with the renderer
// so navigation and drawing can't disagree about where an entry sits.
int vcColumns();
int vcRowsPerColumn();
int vcPageSize();                         // columns * rowsPerColumn
int vcColumnWidth();

// One-line status shown in the bottom bar: either the key hints or the
// result of the last action ("Loaded HELLO", "?File not found", ...).
const char* vcStatusText();
