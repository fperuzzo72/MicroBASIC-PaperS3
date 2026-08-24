#pragma once

#include <cstdint>
#include <cstddef>

// Ported from MicroBASIC's own config.h (editor/port-staging/config.h),
// trimmed to what this bring-up actually uses: there is no menu/file-
// browser/text-editor/BLE/wifi system yet, so UIState is reduced to a single
// state and the corresponding X4 fields (FileInfo, OTA app detection,
// auto-save timing, BLE state) are dropped rather than carried unused.
// Re-add them as those systems get ported.

// --- Key Event (for input queue) ---
struct KeyEvent {
  uint8_t keyCode;
  uint8_t modifiers;
  bool pressed;
};

// --- Buffer/Queue Sizes ---
static constexpr int INPUT_QUEUE_SIZE = 50;

// --- Font IDs (from crosspoint-reader fontIds.h, matching main.cpp's own) ---
#define FONT_BODY  (-1014561631)  // NOTOSANS_14_FONT_ID
#define FONT_UI    (-1559651934)  // NOTOSANS_12_FONT_ID
#define FONT_SMALL (-1246724383)  // UI_10_FONT_ID (ubuntu 10)
#define FONT_LARGE (-1422711852)  // NOTOSANS_16_FONT_ID

// Screen Editor's monospace fonts -- one per SCREEN mode. Same sentinel IDs
// as the X4's own config.h (arbitrary, just need to not collide with the
// four prose fonts above or each other) -- see README's "SCREEN modes"
// table for this device's actual column counts and cell sizes, which differ
// from the X4's (landscape 960x540 here vs the X4's own panel).
#define FONT_SCREEN_MONO_0 (-2000000001)  // SCREEN 0, 32 col
#define FONT_SCREEN_MONO_1 (-2000000002)  // SCREEN 1, 48 col
#define FONT_SCREEN_MONO_2 (-2000000003)  // SCREEN 2, 64 col (default here)
#define FONT_SCREEN_MONO_3 (-2000000004)  // SCREEN 3, 80 col

// --- Screen Editor: max grid size across all 4 SCREEN modes ---
// Actual active cols/rows/cell size depend on the current mode -- see
// screen_editor.h's screenEditorCols()/Rows()/CellW()/CellH(). These two
// just size the underlying storage for the largest mode. On this panel
// that's SCREEN 3 in both axes (80 cols, 22 rows) -- see README.
static constexpr int SCREEN_EDITOR_MAX_COLS = 80;
static constexpr int SCREEN_EDITOR_MAX_ROWS = 22;

// Longest single BASIC line the screen editor will hand to the interpreter.
// Callers size their own stack buffers from this. The interpreter has its own,
// smaller, input buffer (BUFSIZE); this only has to be big enough that nothing
// here truncates before it does.
static constexpr int MAX_PROGRAM_LINE_LEN = 160;

static constexpr int MAX_FILENAME_LEN = 64;

// Reject a browser upload (wifi_sync.cpp) bigger than this -- matches the
// X4's own PROGRAM_UPLOAD_MAX_SIZE. Programs aren't bounded by any editor
// buffer the way notes are (no prose editor is ported here), just by the
// interpreter's own memory, but an unbounded upload is still a bad idea on
// a device this size.
static constexpr size_t PROGRAM_UPLOAD_MAX_SIZE = 16384;

// --- HID Keycodes ---
static constexpr uint8_t HID_KEY_A          = 0x04;
static constexpr uint8_t HID_KEY_C          = 0x06;
static constexpr uint8_t HID_KEY_ENTER      = 0x28;
static constexpr uint8_t HID_KEY_ESCAPE     = 0x29;
static constexpr uint8_t HID_KEY_BACKSPACE  = 0x2A;
static constexpr uint8_t HID_KEY_RIGHT      = 0x4F;
static constexpr uint8_t HID_KEY_LEFT       = 0x50;
static constexpr uint8_t HID_KEY_DOWN       = 0x51;
static constexpr uint8_t HID_KEY_UP         = 0x52;
static constexpr uint8_t HID_KEY_HOME       = 0x4A;
static constexpr uint8_t HID_KEY_END        = 0x4D;
static constexpr uint8_t HID_KEY_PAGE_UP    = 0x4B;
static constexpr uint8_t HID_KEY_PAGE_DOWN  = 0x4E;

// --- HID Modifier Masks ---
static constexpr uint8_t MOD_CTRL_LEFT   = 0x01;
static constexpr uint8_t MOD_SHIFT_LEFT  = 0x02;
static constexpr uint8_t MOD_ALT_LEFT    = 0x04;
static constexpr uint8_t MOD_CTRL_RIGHT  = 0x10;
static constexpr uint8_t MOD_SHIFT_RIGHT = 0x20;
static constexpr uint8_t MOD_ALT_RIGHT   = 0x40;

inline bool isCtrl(uint8_t mod) {
  return (mod & MOD_CTRL_LEFT) || (mod & MOD_CTRL_RIGHT);
}
inline bool isShift(uint8_t mod) {
  return (mod & MOD_SHIFT_LEFT) || (mod & MOD_SHIFT_RIGHT);
}
