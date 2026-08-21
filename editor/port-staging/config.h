#pragma once

#include <cstdint>
#include <cstddef>

// --- UI State Machine ---
enum class UIState {
  MAIN_MENU,
  FILE_BROWSER,
  TEXT_EDITOR,
  RENAME_FILE,
  NEW_FILE,
  SETTINGS,
  BLUETOOTH_SETTINGS,
  PAIRED_KEYBOARDS,
  WIFI_SYNC,
  SCREEN_EDITOR,
  VC_BROWSER  // full-screen program picker, opened by the VC command
};

// --- Display Orientation ---
// Values map to GfxRenderer::Orientation enum
enum class Orientation : uint8_t {
  PORTRAIT = 0,
  LANDSCAPE_CW = 1,    // LandscapeClockwise
  PORTRAIT_INV = 2,     // PortraitInverted
  LANDSCAPE_CCW = 3     // LandscapeCounterClockwise
};

// --- Writing Modes ---
enum class WritingMode : uint8_t {
  NORMAL     = 0,   // Standard scrolling editor
  TYPEWRITER = 1,   // Shows only current line centered on screen
  PAGINATION = 2    // Page-based display instead of scrolling
};

// --- BLE Connection State ---
enum class BLEState : uint8_t {
  DISCONNECTED,
  SCANNING,
  CONNECTING,
  CONNECTED
};

// --- Key Event (for input queue) ---
struct KeyEvent {
  uint8_t keyCode;
  uint8_t modifiers;
  bool pressed;
};

// --- File Info ---
static constexpr int MAX_FILENAME_LEN = 64;
static constexpr int MAX_TITLE_LEN = 40;

struct FileInfo {
  char filename[MAX_FILENAME_LEN];
  char title[MAX_TITLE_LEN];
  unsigned long modTime;
};

// --- Auto-save timing ---
static constexpr unsigned long AUTO_SAVE_IDLE_MS = 10000;    // Save after 10s of no keystrokes
static constexpr unsigned long AUTO_SAVE_MAX_MS  = 120000;   // Hard cap: save every 2min during continuous typing

// --- OTA App Detection ---
static constexpr int MAX_OTA_APPS = 4;

struct OtaAppEntry {
  char name[32];
  int partitionSubtype;
};

extern OtaAppEntry otaApps[];
extern int otaAppCount;

// --- Buffer/Queue Sizes ---
//
// TEXT_BUFFER_SIZE is a hard ceiling on note size, not a function of free
// RAM: text_editor.cpp's textBuffer is a fixed static array of exactly this
// size, and there is no SD-backed paging/streaming for notes larger than
// it — the whole note is always resident in RAM or not loaded at all.
// file_manager.cpp's loadFile() reads at most TEXT_BUFFER_SIZE - 1 bytes
// from disk; a file already larger than that gets silently truncated on
// open, with no warning shown. Worse: saveCurrentFile() then writes exactly
// that truncated buffer back to the original path (through the normal
// write-verify + .bak rotation), so editing and saving a too-large note
// permanently drops everything past the truncation point after the first
// save (a second save even overwrites the one-generation .bak). This can
// only currently happen via a file placed on the SD card from outside the
// device (e.g. WiFi sync's PC→device direction doesn't exist, but manually
// copying a large .txt into /notes/ does) — TODO: revisit, either warn on
// truncated load or support paging a large file in chunks.
static constexpr size_t TEXT_BUFFER_SIZE = 16384;
static constexpr int MAX_FILES = 50;
static constexpr int INPUT_QUEUE_SIZE = 50;
static constexpr int MAX_LINES = 1024;

// --- Font IDs (from crosspoint-reader fontIds.h) ---
#define FONT_BODY    (-1014561631)   // NOTOSANS_14_FONT_ID
#define FONT_UI      (-1559651934)   // NOTOSANS_12_FONT_ID
#define FONT_SMALL   (-1246724383)   // UI_10_FONT_ID (ubuntu 10)
#define FONT_LARGE   (-1422711852)   // NOTOSANS_16_FONT_ID

// Screen Editor's monospace fonts -- one per SCREEN mode (see MicroBASIC
// repo's docs/DEVELOPMENT_LOG.md for the full spec). Arbitrary IDs, just
// need to not collide with the four prose-editor fonts above or each other.
#define FONT_SCREEN_MONO_0 (-2000000001)  // SCREEN 0, 32 col, 24x48
#define FONT_SCREEN_MONO_1 (-2000000002)  // SCREEN 1, 48 col, 16x32 (default)
#define FONT_SCREEN_MONO_2 (-2000000003)  // SCREEN 2, 64 col, 12x24
#define FONT_SCREEN_MONO_3 (-2000000004)  // SCREEN 3, 80 col, 10x20

// --- Screen Editor: max grid size across all 4 SCREEN modes ---
// Actual active cols/rows/cell size depend on the current mode -- see
// screen_editor.h's screenEditorCols()/Rows()/CellW()/CellH(). These two
// just size the underlying storage for the largest mode (SCREEN 3).
static constexpr int SCREEN_EDITOR_MAX_COLS = 80;
static constexpr int SCREEN_EDITOR_MAX_ROWS = 24;

// Longest single BASIC line the screen editor will hand to the interpreter.
// Callers size their own stack buffers from this. The interpreter has its own,
// smaller, input buffer (BUFSIZE); this only has to be big enough that nothing
// here truncates before it does.
static constexpr int MAX_PROGRAM_LINE_LEN = 160;

// Biggest .bas the web uploader accepts.
//
// This used to be 4096, inherited from a whole-program *text buffer* that no
// longer exists -- nothing in this firmware holds a program in RAM as text
// any more, and the upload streams straight to the SD card a chunk at a time.
// So the old number guarded nothing and simply rejected files: the twelve-
// level sokoban is 5.7KB and bounced with "File too large".
//
// The real ceiling on a program is the interpreter's own memory, where it
// lives tokenised -- MEMSIZE, 16KB (patches/tinybasic/03). Tokenised is
// smaller than source, so a file up to that size is a safe bound: anything
// this lets through and the interpreter still cannot hold will fail loudly at
// LOAD rather than silently here.
static constexpr size_t PROGRAM_UPLOAD_MAX_SIZE = 16384;

// --- Font Size ---
enum class FontSize : uint8_t { SMALL = 0, MEDIUM = 1, LARGE = 2 };

inline int editorFontId(FontSize size) {
  switch (size) {
    case FontSize::SMALL:  return FONT_UI;    // notosans 12
    case FontSize::MEDIUM: return FONT_BODY;  // notosans 14
    default:               return FONT_LARGE; // notosans 16
  }
}

// --- HID Keycodes ---
static constexpr uint8_t HID_KEY_A          = 0x04;
static constexpr uint8_t HID_KEY_B          = 0x05;
static constexpr uint8_t HID_KEY_C          = 0x06;
static constexpr uint8_t HID_KEY_D          = 0x07;
static constexpr uint8_t HID_KEY_V          = 0x19;
static constexpr uint8_t HID_KEY_X          = 0x1B;
static constexpr uint8_t HID_KEY_F          = 0x09;
static constexpr uint8_t HID_KEY_N          = 0x11;
static constexpr uint8_t HID_KEY_L          = 0x0F;  // Ctrl+L: clean mode
static constexpr uint8_t HID_KEY_P          = 0x13;
static constexpr uint8_t HID_KEY_Q          = 0x14;
static constexpr uint8_t HID_KEY_R          = 0x15;
static constexpr uint8_t HID_KEY_W          = 0x1A;
static constexpr uint8_t HID_KEY_S          = 0x16;
static constexpr uint8_t HID_KEY_T          = 0x17;
static constexpr uint8_t HID_KEY_Z          = 0x1D;
static constexpr uint8_t HID_KEY_ENTER      = 0x28;
static constexpr uint8_t HID_KEY_ESCAPE     = 0x29;
static constexpr uint8_t HID_KEY_BACKSPACE  = 0x2A;
static constexpr uint8_t HID_KEY_TAB        = 0x2B;
static constexpr uint8_t HID_KEY_SPACE      = 0x2C;
static constexpr uint8_t HID_KEY_DELETE     = 0x4C;
static constexpr uint8_t HID_KEY_RIGHT      = 0x4F;
static constexpr uint8_t HID_KEY_LEFT       = 0x50;
static constexpr uint8_t HID_KEY_DOWN       = 0x51;
static constexpr uint8_t HID_KEY_UP         = 0x52;
static constexpr uint8_t HID_KEY_HOME       = 0x4A;
static constexpr uint8_t HID_KEY_END        = 0x4D;
static constexpr uint8_t HID_KEY_PAGE_UP    = 0x4B;
static constexpr uint8_t HID_KEY_PAGE_DOWN  = 0x4E;
static constexpr uint8_t HID_KEY_CAPSLOCK   = 0x39;
static constexpr uint8_t HID_KEY_F2         = 0x3B;

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

// --- Debug Logging ---
// Define RELEASE_BUILD in platformio.ini to disable all serial output.
// This saves significant power by keeping the UART peripheral inactive.
#ifdef RELEASE_BUILD
  #define DBG_INIT()
  #define DBG_PRINTF(fmt, ...)
  #define DBG_PRINTLN(s)
  #define DBG_PRINT(s)
#else
  #define DBG_INIT()            Serial.begin(115200)
  #define DBG_PRINTF(fmt, ...)  Serial.printf(fmt, ##__VA_ARGS__)
  #define DBG_PRINTLN(s)        Serial.println(s)
  #define DBG_PRINT(s)          Serial.print(s)
#endif
