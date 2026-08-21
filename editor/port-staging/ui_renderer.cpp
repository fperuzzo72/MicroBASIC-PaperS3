#include "ui_renderer.h"
#include "config.h"
#include <algorithm>
#include "text_editor.h"
#include "file_manager.h"
#include "tb_bridge.h"
#include "ble_keyboard.h"
#include "wifi_sync.h"
#include "screen_editor.h"
#include "vc_browser.h"
#include <Utf8.h>

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalDisplay.h>
#include <EpdFont.h>
#include <EpdFontFamily.h>

// External variables
extern bool autoReconnectEnabled;
extern bool darkMode;
extern bool cleanMode;
extern bool deleteConfirmPending;
extern WritingMode writingMode;
extern FontSize fontSize;
extern bool showWordCount;
extern bool remapButtonsInPrograms;

// External functions
uint32_t getCurrentPasskey();
bool isDeviceScanning();
uint32_t getScanAgeMs();

// Font data includes
#include <builtinFonts/notosans_16_regular.h>
#include <builtinFonts/notosans_16_bold.h>
#include <builtinFonts/notosans_14_regular.h>
#include <builtinFonts/notosans_14_bold.h>
#include <builtinFonts/notosans_12_regular.h>
#include <builtinFonts/notosans_12_bold.h>
#include <builtinFonts/ubuntu_10_regular.h>
#include <builtinFonts/ubuntu_10_bold.h>
#include <builtinFonts/unscii_24x48.h>
#include <builtinFonts/unscii_16x32.h>
#include <builtinFonts/unscii_12x24.h>
#include <builtinFonts/unscii_10x20.h>

// Font objects (file-scoped)
static EpdFont ns16Regular(&notosans_16_regular);
static EpdFont ns16Bold(&notosans_16_bold);
static EpdFontFamily ns16Family(&ns16Regular, &ns16Bold);

static EpdFont ns14Regular(&notosans_14_regular);
static EpdFont ns14Bold(&notosans_14_bold);
static EpdFontFamily ns14Family(&ns14Regular, &ns14Bold);

static EpdFont ns12Regular(&notosans_12_regular);
static EpdFont ns12Bold(&notosans_12_bold);
static EpdFontFamily ns12Family(&ns12Regular, &ns12Bold);

static EpdFont u10Regular(&ubuntu_10_regular);
static EpdFont u10Bold(&ubuntu_10_bold);
static EpdFontFamily u10Family(&u10Regular, &u10Bold);

// MicroBASIC SCREEN 0/1/2/3 monospace fonts -- no bold variant (see
// research/fonts/tools/emit_epdfont_header.py in the MicroBASIC repo for
// how these headers were generated from Unscii).
static EpdFont unsciiMono0Regular(&unscii_24x48);
static EpdFontFamily unsciiMono0Family(&unsciiMono0Regular);
static EpdFont unsciiMono1Regular(&unscii_16x32);
static EpdFontFamily unsciiMono1Family(&unsciiMono1Regular);
static EpdFont unsciiMono2Regular(&unscii_12x24);
static EpdFontFamily unsciiMono2Family(&unsciiMono2Regular);
static EpdFont unsciiMono3Regular(&unscii_10x20);
static EpdFontFamily unsciiMono3Family(&unsciiMono3Regular);

// OTA app detection (defined in main.cpp)
extern OtaAppEntry otaApps[];
extern int otaAppCount;

// Extern shared state (defined in main.cpp)
extern UIState currentState;
extern int mainMenuSelection;
extern int selectedFileIndex;
extern int settingsSelection;
extern int bluetoothDeviceSelection;
extern int pairedKeyboardSelection;
extern Orientation currentOrientation;
extern char renameBuffer[];
extern int renameBufferLen;

// Maps an editorFontId(fontSize) result back to the EpdFontFamily object
// that owns it, so editorGlyphWidthPx can look up glyph metrics directly —
// no GfxRenderer/display state needed for that, just the font data itself.
static const EpdFontFamily& editorFontFamily(int fontId) {
  if (fontId == FONT_LARGE) return ns16Family;
  if (fontId == FONT_BODY) return ns14Family;
  if (fontId == FONT_UI) return ns12Family;
  return u10Family;
}

// Per-codepoint pixel width for the word-wrap pass (see text_editor.h's
// editorSetGlyphWidthFn). Reads the live `fontSize` each call so a font-size
// change takes effect on the next wrap recompute without re-registering.
static int editorGlyphWidthPx(uint32_t codepoint) {
  const EpdGlyph* g = editorFontFamily(editorFontId(fontSize)).getGlyph(codepoint);
  return g ? g->advanceX : 8;
}

void rendererSetup(GfxRenderer& renderer) {
  renderer.insertFont(FONT_LARGE, ns16Family);
  renderer.insertFont(FONT_BODY, ns14Family);
  renderer.insertFont(FONT_UI, ns12Family);
  renderer.insertFont(FONT_SMALL, u10Family);
  renderer.insertFont(FONT_SCREEN_MONO_0, unsciiMono0Family);
  renderer.insertFont(FONT_SCREEN_MONO_1, unsciiMono1Family);
  renderer.insertFont(FONT_SCREEN_MONO_2, unsciiMono2Family);
  renderer.insertFont(FONT_SCREEN_MONO_3, unsciiMono3Family);
  editorSetGlyphWidthFn(&editorGlyphWidthPx);
}

// ---------------------------------------------------------------------------
// Clipped draw helpers — use renderer.truncatedText() so NO pixel ever
// exceeds screen width.  This is how crosspoint-reader prevents GFX errors.
// ---------------------------------------------------------------------------

// Draw text that is guaranteed not to overflow the screen width.
// maxW = available pixel width from x to right edge (caller computes).
// Falls back to sw - x - 5 if maxW <= 0.
static void drawClippedText(GfxRenderer& r, int font, int x, int y,
                            const char* text, int maxW = 0,
                            bool black = true,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  if (!text || !text[0]) return;
  int sw = r.getScreenWidth();
  int sh = r.getScreenHeight();
  if (x < 0 || x >= sw || y < 0 || y >= sh) return;

  if (maxW <= 0) maxW = sw - x - 5;   // 5px right margin
  if (maxW <= 0) return;

  auto clipped = r.truncatedText(font, text, maxW, style);
  if (!clipped.empty()) {
    r.drawText(font, x, y, clipped.c_str(), black, style);
  }
}

// Draw right-aligned text (e.g. battery %, RSSI, settings values).
// Computes its own X from the measured text width.
static void drawRightText(GfxRenderer& r, int font, int rightEdge, int y,
                          const char* text, bool black = true,
                          EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  if (!text || !text[0]) return;
  // Use getTextWidth (bounding box) — same measurement truncatedText uses —
  // so the allocated space always matches what the truncation check expects.
  int tw = r.getTextWidth(font, text, style);
  if (tw <= 0) tw = 30;                    // safe fallback
  int x = rightEdge - tw;
  if (x < 5) x = 5;                        // don't go off left edge
  drawClippedText(r, font, x, y, text, rightEdge - x, black, style);
}

// Safe line — just clamp to screen
static void clippedLine(GfxRenderer& r, int x1, int y1, int x2, int y2,
                        bool state = true) {
  int sw = r.getScreenWidth();
  int sh = r.getScreenHeight();
  // Clamp rather than reject
  auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
  x1 = clamp(x1, 0, sw - 1);
  x2 = clamp(x2, 0, sw - 1);
  y1 = clamp(y1, 0, sh - 1);
  y2 = clamp(y2, 0, sh - 1);
  r.drawLine(x1, y1, x2, y2, state);
}

// Safe fillRect — clamp dimensions to screen
static void clippedFillRect(GfxRenderer& r, int x, int y, int w, int h,
                            bool state = true) {
  int sw = r.getScreenWidth();
  int sh = r.getScreenHeight();
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > sw) w = sw - x;
  if (y + h > sh) h = sh - y;
  if (w > 0 && h > 0) r.fillRect(x, y, w, h, state);
}

// ---------------------------------------------------------------------------
// Helper: draw battery percentage in top-right
// ---------------------------------------------------------------------------
static void drawBattery(GfxRenderer& renderer, HalGPIO& gpio) {
  int pct = gpio.getBatteryPercentage();
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", pct);
  drawRightText(renderer, FONT_SMALL, renderer.getScreenWidth() - 8, 5, buf, !darkMode);
}

// Helper: draw BLE status
static void drawBleStatus(GfxRenderer& renderer, int x, int y) {
  const char* status = "";
  switch (getConnectionState()) {
    case BLEState::CONNECTED:    status = "KB Connected"; break;
    case BLEState::SCANNING:     status = "Scanning..."; break;
    case BLEState::CONNECTING:   status = "Connecting..."; break;
    case BLEState::DISCONNECTED: status = "KB Disconnected"; break;
  }
  drawClippedText(renderer, FONT_SMALL, x, y, status, 0, !darkMode);
}

// ===========================================================================
// Screen drawing functions
// ===========================================================================

void drawMainMenu(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;  // text color

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  // Title
  renderer.drawCenteredText(FONT_BODY, 30, "MicroBASIC", tc, EpdFontFamily::BOLD);

  // Menu items (base + dynamically detected OTA apps)
  // "MicroBASIC" (the SCREEN 0-3 terminal) leads the list -- it's the
  // project's actual identity going forward, not just another tool. Then two
  // browse/new pairs, one per collection: BASIC programs under
  // /MicroBASIC/programs, then the MicroWriter prose side, unchanged. The
  // order and count here must match input_handler.cpp's MAIN_MENU dispatch --
  // they are indexed by the same mainMenuSelection.
  static const char* baseMenuItems[] = {"MicroBASIC", "Browse Programs", "New Program",
                                        "Browse Files", "New Note", "Settings", "Sync"};
  static constexpr int BASE_MENU_COUNT = 7;
  int menuCount = BASE_MENU_COUNT + otaAppCount;

  // lineH covers FONT_UI's full glyph extent (ascender+descender, ~35px)
  // with margin, so the selection bar never clips the bottom of descenders
  // (g/y/p/q) — same formula used by every list in this file.
  int lineH = renderer.getLineHeight(FONT_UI) + 8;
  int listTop = 90;
  constexpr int footerReserve = 100;  // keep list from drawing into the footer
  int maxVisible = (sh - listTop - footerReserve) / lineH;
  if (maxVisible < 1) maxVisible = 1;
  int startIdx = 0;
  if (menuCount > maxVisible && mainMenuSelection >= maxVisible) {
    startIdx = mainMenuSelection - maxVisible + 1;
  }

  for (int i = startIdx; i < menuCount && (i - startIdx) < maxVisible; i++) {
    int yPos = listTop + ((i - startIdx) * lineH);
    const char* label = (i < BASE_MENU_COUNT) ? baseMenuItems[i] : otaApps[i - BASE_MENU_COUNT].name;
    if (i == mainMenuSelection) {
      clippedFillRect(renderer, 5, yPos - 4, sw - 10, lineH - 2, tc);
      drawClippedText(renderer, FONT_UI, 20, yPos, label, sw - 40, !tc);
    } else {
      drawClippedText(renderer, FONT_UI, 20, yPos, label, sw - 40, tc);
    }
  }

  // Footer
  constexpr int bm = 60;
  if (sh > bm + 40) {
    clippedLine(renderer, 10, sh - bm, sw - 10, sh - bm, tc);
    drawClippedText(renderer, FONT_SMALL, 20, sh - bm + 8, "Arrows: Navigate  Enter: Select", 0, tc);
    drawBleStatus(renderer, 20, sh - bm + 8 + renderer.getLineHeight(FONT_SMALL) + 2);
  }
  drawBattery(renderer, gpio);

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

void drawFileBrowser(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  // Header
  // "Notes" / "Programs" -- the browser is now used for both, and which one
  // you are in is not otherwise visible from a list of filenames.
  drawClippedText(renderer, FONT_SMALL, 10, 5, fileCollectionName(), 0, tc, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, tc);

  int fc = getFileCount();
  int lineH = renderer.getLineHeight(FONT_UI) + 8;
  int listTop = 42;
  int footerH = 28;  // one line of FONT_SMALL with safe bottom margin
  int maxVisible = (sh - listTop - footerH) / lineH;
  int startIdx = 0;
  if (fc > maxVisible && selectedFileIndex >= maxVisible) {
    startIdx = selectedFileIndex - maxVisible + 1;
  }

  if (fc == 0) {
    drawClippedText(renderer, FONT_UI, 20, listTop + 14, "No notes yet.", 0, tc);
    drawClippedText(renderer, FONT_SMALL, 20, listTop + 36, "Press Ctrl+N to create one.", 0, tc);
  }

  FileInfo* files = getFileList();
  for (int i = startIdx; i < fc && (i - startIdx) < maxVisible; i++) {
    int yPos = listTop + (i - startIdx) * lineH;

    if (i == selectedFileIndex) {
      clippedFillRect(renderer, 5, yPos - 4, sw - 10, lineH - 2, tc);
      drawClippedText(renderer, FONT_UI, 15, yPos, files[i].title, sw - 30, !tc);
    } else {
      drawClippedText(renderer, FONT_UI, 15, yPos, files[i].title, sw - 30, tc);
    }
  }

  // Footer
  clippedLine(renderer, 5, sh - footerH - 2, sw - 5, sh - footerH - 2, tc);
  if (deleteConfirmPending && fc > 0) {
    drawClippedText(renderer, FONT_SMALL, 10, sh - footerH + 4, "Delete? Enter:Yes  Esc:No", 0, tc);
  } else {
    drawClippedText(renderer, FONT_SMALL, 10, sh - footerH + 4,
                    "Ctrl+N:Title  Ctrl+D:Delete", 0, tc);
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

// Helper: draw a single editor line from the buffer
static void drawEditorLine(GfxRenderer& renderer, int lineIdx, int x, int yPos,
                           int maxW, bool tc) {
  char* buf = editorGetBuffer();
  size_t bufLen = editorGetLength();
  int totalLines = editorGetLineCount();

  int lineStart = editorGetLinePosition(lineIdx);
  int lineEnd = (lineIdx + 1 < totalLines) ? editorGetLinePosition(lineIdx + 1) : (int)bufLen;
  int dispEnd = lineEnd;
  if (dispEnd > lineStart && buf[dispEnd - 1] == '\n') dispEnd--;

  int len = dispEnd - lineStart;
  if (len <= 0) return;

  char lineBuf[256];
  int copyLen = (len < (int)sizeof(lineBuf) - 1) ? len : (int)sizeof(lineBuf) - 1;
  strncpy(lineBuf, buf + lineStart, copyLen);
  lineBuf[copyLen] = '\0';

  const int fontId = editorFontId(fontSize);

  // Selection range, clamped to this line's displayed (possibly truncated) window.
  int selStart = editorGetSelectionStart();
  int selEnd = editorGetSelectionEnd();
  int segStart = -1, segEnd = -1;
  if (selStart >= 0 && selStart < lineStart + copyLen && selEnd > lineStart) {
    segStart = std::max(selStart, lineStart) - lineStart;
    segEnd = std::min(selEnd, lineStart + copyLen) - lineStart;
  }

  if (segStart < 0 || segStart >= segEnd) {
    drawClippedText(renderer, fontId, x, yPos, lineBuf, maxW, tc);
    return;
  }

  // Selection overlaps this line: draw as up to three segments — plain
  // prefix, inverted-fill selection, plain suffix — same visual language as
  // the file-list selection highlight (filled rect + inverted text).
  char seg[256];
  int lineHeight = renderer.getLineHeight(fontId);

  int prefixW = 0;
  if (segStart > 0) {
    strncpy(seg, lineBuf, segStart);
    seg[segStart] = '\0';
    drawClippedText(renderer, fontId, x, yPos, seg, maxW, tc);
    prefixW = renderer.getTextAdvanceX(fontId, seg);
  }

  int selLen = segEnd - segStart;
  strncpy(seg, lineBuf + segStart, selLen);
  seg[selLen] = '\0';
  int selW = renderer.getTextAdvanceX(fontId, seg);
  int selX = x + prefixW;
  int remainingW = maxW - prefixW;
  if (remainingW > 0) {
    int fillW = std::min(selW, remainingW);
    clippedFillRect(renderer, selX, yPos, fillW, lineHeight, tc);
    drawClippedText(renderer, fontId, selX, yPos, seg, remainingW, !tc);
  }

  if (segEnd < copyLen) {
    int suffixMaxW = maxW - prefixW - selW;
    if (suffixMaxW > 0) {
      drawClippedText(renderer, fontId, selX + selW, yPos, lineBuf + segEnd, suffixMaxW, tc);
    }
  }
}

// Helper: draw cursor at the given screen position
static void drawEditorCursor(GfxRenderer& renderer, int cursorY, int lineHeight,
                             int sw, bool tc) {
  int curLine = editorGetCursorLine();
  int curCol = editorGetCursorCol();
  char* buf = editorGetBuffer();

  int lineStart = editorGetLinePosition(curLine);
  char prefix[256];
  int prefixLen = (curCol < (int)sizeof(prefix) - 1) ? curCol : (int)sizeof(prefix) - 1;
  strncpy(prefix, buf + lineStart, prefixLen);
  prefix[prefixLen] = '\0';

  int cursorX = 10 + renderer.getTextAdvanceX(editorFontId(fontSize), prefix);
  int cursorW = renderer.getSpaceWidth(editorFontId(fontSize));
  if (cursorW < 2) cursorW = 8;

  if (cursorX >= 0 && cursorX + cursorW <= sw && cursorY >= 0 && cursorY + lineHeight <= renderer.getScreenHeight()) {
    renderer.fillRect(cursorX, cursorY, cursorW, lineHeight, tc);
  }
}

// Get the mode indicator string for the current writing mode
static const char* getModeIndicator() {
  switch (writingMode) {
    case WritingMode::TYPEWRITER: return "[T]";
    case WritingMode::PAGINATION: return "[P]";
    default:                      return "[S]";
  }
}

// Helper: draw the standard editor header, returns textAreaTop
// centerText is optional text drawn centered in the header (e.g. "Page 1/3")
static int drawEditorHeader(GfxRenderer& renderer, HalGPIO& gpio, int sw, bool tc,
                            const char* centerText = nullptr) {
  if (cleanMode) return 8;

  const char* title = editorGetCurrentTitle();
  char headerBuf[64];
  if (editorHasUnsavedChanges()) {
    snprintf(headerBuf, sizeof(headerBuf), "%s *", title);
  } else {
    strncpy(headerBuf, title, sizeof(headerBuf) - 1);
    headerBuf[sizeof(headerBuf) - 1] = '\0';
  }
  // Mode indicator — fixed position, right-anchored before battery
  const char* modeInd = getModeIndicator();
  int modeW = renderer.getTextAdvanceX(FONT_SMALL, modeInd);
  int modeX = sw - 70 - modeW;
  drawClippedText(renderer, FONT_SMALL, modeX, 5, modeInd, modeW + 5, tc);

  // Word count — drawn to the left of the mode indicator
  int titleMaxW = modeX - 10;
  if (showWordCount) {
    int wc = editorGetWordCount();
    char wcBuf[24];
    if (wc == 1) snprintf(wcBuf, sizeof(wcBuf), "1 word");
    else         snprintf(wcBuf, sizeof(wcBuf), "%d words", wc);
    int wcW = renderer.getTextAdvanceX(FONT_SMALL, wcBuf);
    int wcX = modeX - 8 - wcW;
    if (wcX > 10) {
      drawClippedText(renderer, FONT_SMALL, wcX, 5, wcBuf, wcW + 5, tc);
      titleMaxW = wcX - 10;
    }
  }

  // Title — stops before word count (or mode indicator if word count hidden)
  drawClippedText(renderer, FONT_SMALL, 10, 5, headerBuf, titleMaxW, tc, EpdFontFamily::BOLD);

  // Centered text (e.g. page indicator)
  if (centerText) {
    int ctW = renderer.getTextAdvanceX(FONT_SMALL, centerText);
    drawClippedText(renderer, FONT_SMALL, (sw - ctW) / 2, 5, centerText, ctW + 5, tc);
  }

  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, tc);
  return 38;
}

void drawTextEditor(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  int lineHeight = renderer.getLineHeight(editorFontId(fontSize));
  if (lineHeight <= 0) lineHeight = 20;
  int totalLines = editorGetLineCount();
  int curLine = editorGetCursorLine();

  // PgUp/PgDn jump size, computed the same way every frame regardless of
  // writing mode (Normal mode's own header height estimate — matches
  // Pagination's own tempTextTop below), so it stays a real "screenful"
  // even in Typewriter mode, where editorSetVisibleLines() below is forced
  // to 1 (only one line is ever drawn there).
  {
    int estTextAreaTop = cleanMode ? 8 : 38;
    int estVisibleLines = (sh - 5 - estTextAreaTop) / lineHeight;
    if (estVisibleLines < 1) estVisibleLines = 1;
    editorSetPageJumpLines(estVisibleLines);
  }

  // --- TYPEWRITER MODE ---
  if (writingMode == WritingMode::TYPEWRITER) {
    // In clean mode (Ctrl+L): just text on blank screen, no header
    int textAreaTop = cleanMode ? 0 : drawEditorHeader(renderer, gpio, sw, tc);

    // Center the current line vertically
    int textAreaHeight = sh - textAreaTop;
    int centerY = textAreaTop + (textAreaHeight / 2) - (lineHeight / 2);

    // Draw only the current line
    if (curLine < totalLines) {
      drawEditorLine(renderer, curLine, 10, centerY, sw - 20, tc);
    }

    // Draw cursor
    drawEditorCursor(renderer, centerY, lineHeight, sw, tc);

    editorSetVisibleLines(1);

    renderer.beginRefresh(HalDisplay::FAST_REFRESH);
    return;
  }

  // --- PAGINATION MODE ---
  if (writingMode == WritingMode::PAGINATION) {
    // Pre-compute page info for the header
    // Use a temporary linesPerPage estimate (will be exact since header height is fixed)
    int tempTextTop = cleanMode ? 8 : 38;
    int tempLinesPerPage = (sh - 5 - tempTextTop) / lineHeight;
    if (tempLinesPerPage < 1) tempLinesPerPage = 1;
    int currentPage = curLine / tempLinesPerPage;
    int totalPages = (totalLines + tempLinesPerPage - 1) / tempLinesPerPage;
    if (totalPages < 1) totalPages = 1;

    char pageStr[16];
    snprintf(pageStr, sizeof(pageStr), "Pg %d/%d", currentPage + 1, totalPages);
    int textAreaTop = drawEditorHeader(renderer, gpio, sw, tc, pageStr);

    int textAreaBottom = sh - 5;
    int textAreaHeight = textAreaBottom - textAreaTop;
    int linesPerPage = textAreaHeight / lineHeight;
    if (linesPerPage < 1) linesPerPage = 1;

    // Recompute with actual linesPerPage if it differs
    currentPage = curLine / linesPerPage;
    int pageStart = currentPage * linesPerPage;

    editorSetVisibleLines(linesPerPage);

    // Draw lines for this page
    for (int i = 0; i < linesPerPage && (pageStart + i) < totalLines; i++) {
      int yPos = textAreaTop + (i * lineHeight);
      drawEditorLine(renderer, pageStart + i, 10, yPos, sw - 20, tc);
    }

    // Draw cursor if on this page
    if (curLine >= pageStart && curLine < pageStart + linesPerPage) {
      int cursorY = textAreaTop + ((curLine - pageStart) * lineHeight);
      drawEditorCursor(renderer, cursorY, lineHeight, sw, tc);
    }

    renderer.beginRefresh(HalDisplay::FAST_REFRESH);
    return;
  }

  // --- NORMAL MODE ---
  int textAreaTop = drawEditorHeader(renderer, gpio, sw, tc);

  int textAreaBottom = sh - 5;
  int textAreaHeight = textAreaBottom - textAreaTop;
  int visibleLines = textAreaHeight / lineHeight;

  editorSetVisibleLines(visibleLines);

  int vpStart = editorGetViewportStart();
  char* buf = editorGetBuffer();
  size_t bufLen = editorGetLength();

  // Draw visible lines
  for (int i = 0; i < visibleLines && (vpStart + i) < totalLines; i++) {
    int yPos = textAreaTop + (i * lineHeight);
    drawEditorLine(renderer, vpStart + i, 10, yPos, sw - 20, tc);
  }

  // Draw cursor
  if (curLine >= vpStart && curLine < vpStart + visibleLines) {
    int cursorY = textAreaTop + ((curLine - vpStart) * lineHeight);
    drawEditorCursor(renderer, cursorY, lineHeight, sw, tc);
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

void drawRenameScreen(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  // Programs are named, notes are titled -- what this field does is genuinely
  // different between the two, so it says which.
  const bool raw = getFileCollection() == FileCollection::PROGRAMS;
  drawClippedText(renderer, FONT_SMALL, 10, 5, raw ? "Edit Filename" : "Edit Title",
                  0, tc, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, tc);

  drawClippedText(renderer, FONT_SMALL, 20, 42,
                  raw ? "Program filename (e.g. pacman.bas):" : "Note title:", 0, tc);
  int boxY = 64, boxH = 36;
  int textY = boxY + 8;
  renderer.drawRect(15, boxY, sw - 30, boxH, tc);
  drawClippedText(renderer, FONT_UI, 20, textY, renameBuffer, sw - 50, tc);

  // Cursor — thin bar aligned with text
  int cursorX = 20 + renderer.getTextAdvanceX(FONT_UI, renameBuffer);
  if (cursorX + 2 < sw - 15)
    renderer.fillRect(cursorX, textY, 2, 16, tc);

  // Footer
  clippedLine(renderer, 5, sh - 36, sw - 5, sh - 36, tc);
  drawClippedText(renderer, FONT_SMALL, 10, sh - 30, "Enter: Confirm   Esc: Cancel", 0, tc);

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

void drawSettingsMenu(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  drawClippedText(renderer, FONT_SMALL, 10, 5, "Settings", 0, !darkMode, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, !darkMode);

  // Setting items: Orientation, Dark Mode, Writing Mode, Font Size, Bluetooth, Paired Keyboards
  static const char* labels[] = {
    "Orientation", "Dark Mode", "Writing Mode", "Font Size",
    "Game Buttons", "Bluetooth", "Paired Keyboards"
  };
  const int SETTINGS_COUNT = 7;

  // Compute line height to fit all items — use smaller spacing if needed.
  // Default matches FONT_UI's real glyph extent (ascender+descender) with
  // margin; only shrinks below that on unusually short screens, in which
  // case the selection bar fills as much of the row as it can rather than
  // clipping by a fixed, possibly-wrong amount.
  int lineH = renderer.getLineHeight(FONT_UI) + 8;
  int listTop = 50;
  if (listTop + SETTINGS_COUNT * lineH > sh - 70) {
    lineH = (sh - 70 - listTop) / SETTINGS_COUNT;
    if (lineH < 24) lineH = 24;
  }

  for (int i = 0; i < SETTINGS_COUNT; i++) {
    int yPos = listTop + (i * lineH);
    bool sel = (i == settingsSelection);

    if (sel) {
      clippedFillRect(renderer, 5, yPos - 3, sw - 10, lineH - 3, !darkMode);
      drawClippedText(renderer, FONT_UI, 15, yPos, labels[i], sw / 2 - 15, darkMode);
    } else {
      drawClippedText(renderer, FONT_UI, 15, yPos, labels[i], sw / 2 - 15, !darkMode);
    }

    // Value on the right
    char val[32] = "";
    if (i == 0) {
      switch (currentOrientation) {
        case Orientation::PORTRAIT:      strcpy(val, "Portrait"); break;
        case Orientation::LANDSCAPE_CW:  strcpy(val, "Landscape CW"); break;
        case Orientation::PORTRAIT_INV:  strcpy(val, "Inverted"); break;
        case Orientation::LANDSCAPE_CCW: strcpy(val, "Landscape CCW"); break;
      }
    } else if (i == 1) {
      strcpy(val, darkMode ? "Dark" : "Light");
    } else if (i == 2) {
      switch (writingMode) {
        case WritingMode::NORMAL:     strcpy(val, "Normal"); break;
        case WritingMode::TYPEWRITER: strcpy(val, "Typewriter"); break;
        case WritingMode::PAGINATION: strcpy(val, "Pagination"); break;
      }
    } else if (i == 3) {
      switch (fontSize) {
        case FontSize::SMALL:  strcpy(val, "Small"); break;
        case FontSize::MEDIUM: strcpy(val, "Medium"); break;
        default:               strcpy(val, "Large"); break;
      }
    } else if (i == 4) {
      // Girados 90 graus para a paisagem do editor de tela, e so enquanto um
      // programa BASIC executa. Ver remapButtonsInPrograms em main.cpp.
      strcpy(val, remapButtonsInPrograms ? "Rotated" : "As labelled");
    } else if (i == 6) {
      int kbCount = getPairedKeyboardCount();
      if (kbCount == 0) strcpy(val, "None");
      else if (kbCount == 1) strcpy(val, "1 keyboard");
      else snprintf(val, sizeof(val), "%d keyboards", kbCount);
    }

    if (val[0] != '\0') {
      drawRightText(renderer, FONT_UI, sw - 20, yPos, val, sel ? darkMode : !darkMode);
    }
  }

  // Footer
  constexpr int bm = 60;
  if (sh > bm + 30) {
    clippedLine(renderer, 10, sh - bm, sw - 10, sh - bm, !darkMode);
    drawClippedText(renderer, FONT_SMALL, 20, sh - bm + 12,
                    "Arrows:Navigate  Enter:Change  Esc:Back", 0, !darkMode);
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

void drawBluetoothSettings(GfxRenderer& renderer, HalGPIO& gpio) {
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();

  renderer.clearScreen();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  // Header
  drawClippedText(renderer, FONT_SMALL, 10, 5, "Bluetooth Devices", 0, tc, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, tc);

  // Connection status
  const char* status = "";
  switch (getConnectionState()) {
    case BLEState::CONNECTED:    status = "Connected to keyboard"; break;
    case BLEState::SCANNING:     status = "Scanning for devices..."; break;
    case BLEState::CONNECTING:   status = "Connecting..."; break;
    case BLEState::DISCONNECTED: status = "Not connected"; break;
  }
  drawClippedText(renderer, FONT_SMALL, 10, 45, status, sw / 2 - 10, tc);

  // Paired device info
  std::string storedAddr, storedName;
  if (getStoredDevice(storedAddr, storedName)) {
    char pairedStr[64];
    snprintf(pairedStr, sizeof(pairedStr), "Paired: %s", storedName.c_str());
    drawClippedText(renderer, FONT_SMALL, sw / 2, 45, pairedStr, sw / 2 - 10, tc);
  }

  // Passkey display
  uint32_t passkey = getCurrentPasskey();
  // Where the scan-status / "Searching.../Found:" line (if any) ends — the
  // device list below must start after this, not at a fixed y=70, or it
  // draws on top of that text whenever a scan is in progress (which is the
  // common case, since devices appear *while* still scanning).
  int scanStatusBottom = 70;
  if (passkey > 0) {
    char passkeyStr[32];
    drawClippedText(renderer, FONT_UI, 20, 100, "PAIRING CODE:", 0, tc, EpdFontFamily::BOLD);
    snprintf(passkeyStr, sizeof(passkeyStr), "%06lu", passkey);
    drawClippedText(renderer, FONT_BODY, 20, 130, passkeyStr, 0, tc, EpdFontFamily::BOLD);
    drawClippedText(renderer, FONT_SMALL, 20, 160, "Type this code on your keyboard", 0, tc);
    drawClippedText(renderer, FONT_SMALL, 20, 180, "then press Enter", 0, tc);
  } else if (isDeviceScanning()) {
    static uint8_t dotPhase = 0;
    static uint32_t lastAnimMs = 0;
    if (millis() - lastAnimMs > 900) {
      dotPhase = (dotPhase + 1) % 4;
      lastAnimMs = millis();
    }
    std::string dots(dotPhase, '.');
    char scanningStr[64];
    int deviceCount = getDiscoveredDeviceCount();
    snprintf(scanningStr, sizeof(scanningStr), "Searching for devices%s", dots.c_str());
    drawClippedText(renderer, FONT_SMALL, 10, 60, scanningStr, sw / 2 - 10, tc);

    char foundStr[32];
    snprintf(foundStr, sizeof(foundStr), "Found: %d", deviceCount);
    drawClippedText(renderer, FONT_SMALL, sw / 2, 60, foundStr, sw / 2 - 10, tc);

    scanStatusBottom = 60 + renderer.getLineHeight(FONT_SMALL) + 12;
  }

  // Device list — skipped during passkey entry (that UI owns this screen
  // area instead), and starts below whatever scan-status text was drawn
  // above so it never overlaps it.
  int deviceCount = getDiscoveredDeviceCount();
  if (deviceCount > 0 && passkey == 0) {
    BleDeviceInfo* devices = getDiscoveredDevices();

    char headerStr[64];
    snprintf(headerStr, sizeof(headerStr), "Available devices: %d", deviceCount);
    drawClippedText(renderer, FONT_SMALL, 10, scanStatusBottom, headerStr, 0, tc, EpdFontFamily::BOLD);

    // Show as many devices as fit — lineH covers FONT_UI's full glyph
    // extent (ascender+descender, ~35px) with margin, so the selection
    // highlight never clips the bottom of descenders (g/y/p/q). Same
    // formula used by every list in this file.
    int listTop = scanStatusBottom + renderer.getLineHeight(FONT_SMALL) + 12;
    int lineH = renderer.getLineHeight(FONT_UI) + 8;
    int footerZone = 100;  // keep in sync with the "yPos > sh - 100" check below
    int maxDevicesToShow = (sh - listTop - footerZone) / lineH;
    if (maxDevicesToShow < 1) maxDevicesToShow = 1;
    int startIndex = 0;
    if (bluetoothDeviceSelection >= maxDevicesToShow) {
      startIndex = bluetoothDeviceSelection - maxDevicesToShow + 1;
    }
    int devicesToShow = (deviceCount - startIndex < maxDevicesToShow)
                        ? deviceCount - startIndex : maxDevicesToShow;

    for (int i = 0; i < devicesToShow; i++) {
      int deviceIndex = startIndex + i;
      int yPos = listTop + (i * lineH);

      // Stop drawing if we'd go into the footer zone
      if (yPos > sh - footerZone) break;

      bool isSelected = (bluetoothDeviceSelection == deviceIndex);
      bool isConnected = (getCurrentDeviceAddress() == devices[deviceIndex].address);

      const char* displayName = devices[deviceIndex].name.empty()
                                ? devices[deviceIndex].address.c_str()
                                : devices[deviceIndex].name.c_str();

      // Available width: leave room for RSSI on the right (~80px)
      int nameMaxW = sw - 100;

      if (isSelected || isConnected) {
        clippedFillRect(renderer, 5, yPos - 4, sw - 10, lineH - 2, tc);
        drawClippedText(renderer, FONT_UI, 15, yPos, displayName, nameMaxW, !tc);
      } else {
        drawClippedText(renderer, FONT_UI, 15, yPos, displayName, nameMaxW, tc);
      }

      // RSSI on the right
      char rssiStr[16];
      snprintf(rssiStr, sizeof(rssiStr), "%ddBm", devices[deviceIndex].rssi);
      drawRightText(renderer, FONT_SMALL, sw - 10, yPos, rssiStr, tc);
    }

    // Page indicator
    if (deviceCount > maxDevicesToShow) {
      char navHint[32];
      int pageNum = (bluetoothDeviceSelection / maxDevicesToShow) + 1;
      int totalPages = (deviceCount + maxDevicesToShow - 1) / maxDevicesToShow;
      snprintf(navHint, sizeof(navHint), "Page %d/%d", pageNum, totalPages);
      int navY = listTop + (devicesToShow * lineH);
      if (navY < sh - footerZone)
        drawClippedText(renderer, FONT_SMALL, 15, navY, navHint, 0, tc);
    }
  } else {
    drawClippedText(renderer, FONT_UI, 20, 80, "No devices found", 0, tc);
    drawClippedText(renderer, FONT_SMALL, 20, 100, "Press Enter to scan for devices", 0, tc);
  }

  // Footer
  constexpr int bm = 60;
  if (sh > bm + 30) {
    clippedLine(renderer, 10, sh - bm, sw - 10, sh - bm, tc);
    int footerLineH = renderer.getLineHeight(FONT_SMALL) + 2;
    drawClippedText(renderer, FONT_SMALL, 10, sh - bm + 8, "Enter:Connect  Right:Scan", 0, tc);
    drawClippedText(renderer, FONT_SMALL, 10, sh - bm + 8 + footerLineH, "Left:Disconnect  Esc:Back", 0, tc);
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

void drawPairedKeyboardsMenu(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  drawClippedText(renderer, FONT_SMALL, 10, 5, "Paired Keyboards", 0, tc, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, tc);

  int count = getPairedKeyboardCount();
  if (count == 0) {
    drawClippedText(renderer, FONT_UI, 20, 60, "No paired keyboards", 0, tc);
    drawClippedText(renderer, FONT_SMALL, 20, 85, "Go to Bluetooth to scan and connect", 0, tc);
  } else {
    std::string currentAddr = getCurrentDeviceAddress();
    // lineH covers FONT_UI's full glyph extent (ascender+descender) with
    // margin — same formula used by every list in this file.
    int lineH = renderer.getLineHeight(FONT_UI) + 8;
    int listTop = 44;
    constexpr int bm = 60;
    int footerZone = bm + 8;  // keep list from drawing into the footer below
    int maxVisible = (sh - listTop - footerZone) / lineH;
    if (maxVisible < 1) maxVisible = 1;
    int startIdx = 0;
    if (count > maxVisible && pairedKeyboardSelection >= maxVisible) {
      startIdx = pairedKeyboardSelection - maxVisible + 1;
    }

    for (int i = startIdx; i < count && (i - startIdx) < maxVisible; i++) {
      std::string addr, name; uint8_t addrType;
      getPairedKeyboard(i, addr, name, addrType);

      int yPos = listTop + ((i - startIdx) * lineH);
      bool sel = (i == pairedKeyboardSelection);
      bool active = (!currentAddr.empty() && currentAddr == addr);

      if (sel) {
        clippedFillRect(renderer, 5, yPos - 4, sw - 10, lineH - 2, tc);
        drawClippedText(renderer, FONT_UI, 15, yPos, name.c_str(), sw - 90, !tc);
      } else {
        drawClippedText(renderer, FONT_UI, 15, yPos, name.c_str(), sw - 90, tc);
      }

      if (active) {
        drawRightText(renderer, FONT_SMALL, sw - 10, yPos + 2, "active", sel ? !tc : tc);
      } else if (!active && i == getLastUsedKeyboardIndex() && currentAddr.empty()) {
        drawRightText(renderer, FONT_SMALL, sw - 10, yPos + 2, "last", sel ? !tc : tc);
      }
    }
  }

  constexpr int bm = 60;
  if (sh > bm + 30) {
    clippedLine(renderer, 10, sh - bm, sw - 10, sh - bm, tc);
    int footerLineH = renderer.getLineHeight(FONT_SMALL) + 2;
    drawClippedText(renderer, FONT_SMALL, 10, sh - bm + 8, "Enter:Connect  D:Forget", 0, tc);
    drawClippedText(renderer, FONT_SMALL, 10, sh - bm + 8 + footerLineH, "Left:Disconnect  Esc:Back", 0, tc);
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

// Helper: draw signal strength indicator (1-4 bars)
static void drawSignalBars(GfxRenderer& r, int x, int y, int rssi, bool color) {
  // RSSI to bars: > -50 = 4, > -65 = 3, > -75 = 2, else 1
  int bars = (rssi > -50) ? 4 : (rssi > -65) ? 3 : (rssi > -75) ? 2 : 1;
  for (int i = 0; i < 4; i++) {
    int bh = 4 + i * 3;  // bar heights: 4, 7, 10, 13
    int by = y + 13 - bh;
    if (i < bars) {
      clippedFillRect(r, x + i * 5, by, 3, bh, color);
    } else {
      clippedFillRect(r, x + i * 5, by + bh - 2, 3, 2, color);
    }
  }
}

void drawSyncScreen(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  // Header
  drawClippedText(renderer, FONT_SMALL, 10, 5, "Sync", 0, tc, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, tc);

  SyncState state = getSyncState();

  switch (state) {
    case SyncState::SCANNING: {
      drawClippedText(renderer, FONT_UI, 20, 80, "Scanning for networks...", sw - 40, tc);
      break;
    }

    case SyncState::NETWORK_LIST: {
      int nc = getNetworkCount();
      int sel = getSelectedNetwork();

      if (nc == 0) {
        const char* st = getSyncStatusText();
        drawClippedText(renderer, FONT_UI, 20, 60, st[0] ? st : "No networks found", sw - 40, tc);
        drawClippedText(renderer, FONT_SMALL, 20, 90, "Enter: Rescan  Esc: Back", 0, tc);
      } else {
        drawClippedText(renderer, FONT_SMALL, 10, 38, "Select network:", 0, tc);

        int lineH = renderer.getLineHeight(FONT_UI) + 8;
        int listTop = 56;
        int footerH = 28;
        int maxVisible = (sh - listTop - footerH) / lineH;
        int startIdx = 0;
        if (nc > maxVisible && sel >= maxVisible) {
          startIdx = sel - maxVisible + 1;
        }

        for (int i = startIdx; i < nc && (i - startIdx) < maxVisible; i++) {
          int yPos = listTop + (i - startIdx) * lineH;
          bool isSel = (i == sel);

          // Build display string: signal indicator + lock + saved + SSID
          char label[48];
          snprintf(label, sizeof(label), "%s%s%s",
                   isNetworkEncrypted(i) ? "* " : "  ",
                   isNetworkSaved(i) ? "+ " : "",
                   getNetworkSSID(i));

          if (isSel) {
            clippedFillRect(renderer, 5, yPos - 4, sw - 10, lineH - 2, tc);
            drawClippedText(renderer, FONT_UI, 15, yPos, label, sw - 50, !tc);
            drawSignalBars(renderer, sw - 30, yPos, getNetworkRSSI(i), !tc);
          } else {
            drawClippedText(renderer, FONT_UI, 15, yPos, label, sw - 50, tc);
            drawSignalBars(renderer, sw - 30, yPos, getNetworkRSSI(i), tc);
          }
        }
      }

      // Footer
      constexpr int bm = 28;
      clippedLine(renderer, 10, sh - bm - 2, sw - 10, sh - bm - 2, tc);
      drawClippedText(renderer, FONT_SMALL, 10, sh - bm + 4,
                      "*=encrypted +=saved  Enter:Select  Esc:Back", 0, tc);
      break;
    }

    case SyncState::PASSWORD_ENTRY: {
      int sel = getSelectedNetwork();
      char heading[48];
      snprintf(heading, sizeof(heading), "Password for %s", getNetworkSSID(sel));
      drawClippedText(renderer, FONT_SMALL, 20, 42, heading, sw - 40, tc);

      // Password field box
      renderer.drawRect(15, 62, sw - 30, 30, tc);

      // Show dots for password characters (privacy)
      int pLen = getPasswordLen();
      char dots[64];
      for (int i = 0; i < pLen; i++) dots[i] = '*';
      dots[pLen] = '\0';
      drawClippedText(renderer, FONT_UI, 20, 66, dots, sw - 50, tc);

      // Cursor
      int cursorX = 20 + renderer.getTextAdvanceX(FONT_UI, dots);
      int cursorW = renderer.getSpaceWidth(FONT_UI);
      if (cursorW < 2) cursorW = 8;
      if (cursorX + cursorW < sw)
        renderer.fillRect(cursorX, 66, cursorW, 20, tc);

      drawClippedText(renderer, FONT_SMALL, 20, 110, "Enter: Connect   Esc: Cancel", 0, tc);
      break;
    }

    case SyncState::CONNECTING: {
      const char* st = getSyncStatusText();
      drawClippedText(renderer, FONT_UI, 20, 80, st, sw - 40, tc);
      drawClippedText(renderer, FONT_SMALL, 20, 110, "Esc: Cancel", 0, tc);
      break;
    }

    case SyncState::SYNCING: {
      const char* ip = getSyncStatusText();
      drawClippedText(renderer, FONT_SMALL, 20, 42, ip, sw - 40, tc, EpdFontFamily::BOLD);

      int sent    = getSyncFilesSent();
      bool pcConn = isPcConnected();

      // Build the stage list. Static so string pointers remain valid after this scope.
      static char fileStageText[MAX_FILES][16];
      static struct { const char* text; bool done; } stages[MAX_FILES + 3];
      int numStages = 0;

      auto push = [&](const char* text, bool done) {
        if (numStages < MAX_FILES + 3) stages[numStages++] = { text, done };
      };

      push("Connected to WiFi", true);
      push("PC connected",      pcConn);

      if (pcConn) {
        for (int i = 0; i < sent && i < MAX_FILES; i++) {
          snprintf(fileStageText[i], sizeof(fileStageText[i]), "File %d sent", i + 1);
          push(fileStageText[i], true);
        }
        push("Sync complete", false);
      }

      // Display with auto-scroll: keep last [x] + next [-] in view
      // FONT_SMALL's real line spacing (advanceY) is 24px — 22 was 2px short.
      const int lineH = renderer.getLineHeight(FONT_SMALL) + 2;
      constexpr int listTop = 62;
      constexpr int footerH = 32;
      int maxVisible = (sh - listTop - footerH) / lineH;

      int lastDone = 0;
      for (int i = 0; i < numStages; i++) {
        if (stages[i].done) lastDone = i;
      }
      int startIdx = 0;
      if (numStages > maxVisible) {
        startIdx = lastDone - maxVisible + 2;
        if (startIdx < 0) startIdx = 0;
        if (startIdx + maxVisible > numStages) startIdx = numStages - maxVisible;
      }

      for (int i = startIdx; i < numStages && (i - startIdx) < maxVisible; i++) {
        int yPos = listTop + (i - startIdx) * lineH;
        char line[52];
        snprintf(line, sizeof(line), "%s %s",
                 stages[i].done ? "[x]" : "[-]", stages[i].text);
        drawClippedText(renderer, FONT_SMALL, 15, yPos, line, sw - 25, tc);
      }

      // Footer
      constexpr int bm = 28;
      clippedLine(renderer, 10, sh - bm - 2, sw - 10, sh - bm - 2, tc);
      char countStr[32];
      snprintf(countStr, sizeof(countStr), "Sent: %d   Esc: Cancel", sent);
      drawClippedText(renderer, FONT_SMALL, 10, sh - bm + 4, countStr, sw - 20, tc);
      break;
    }

    case SyncState::DONE: {
      const char* summary = getSyncStatusText();
      drawClippedText(renderer, FONT_SMALL, 20, 50, "Sync Complete", 0, tc, EpdFontFamily::BOLD);
      drawClippedText(renderer, FONT_UI, 20, 85, summary, sw - 40, tc);
      drawClippedText(renderer, FONT_SMALL, 20, 125, "Returning to menu...", 0, tc);
      break;
    }

    case SyncState::CONNECT_FAILED: {
      drawClippedText(renderer, FONT_UI, 20, 80, "Connection failed", sw - 40, tc);
      drawClippedText(renderer, FONT_SMALL, 20, 110, "Enter: Retry   Esc: Back", 0, tc);
      break;
    }

    case SyncState::SAVE_PROMPT: {
      const char* ip = getSyncStatusText();
      drawClippedText(renderer, FONT_SMALL, 20, 50, "Connected!", 0, tc, EpdFontFamily::BOLD);
      drawClippedText(renderer, FONT_UI, 20, 80, ip, sw - 40, tc);
      drawClippedText(renderer, FONT_SMALL, 20, 120, "Save password?", 0, tc, EpdFontFamily::BOLD);
      drawClippedText(renderer, FONT_SMALL, 20, 145, "Enter/Up: Yes   Down/Esc: No", 0, tc);
      break;
    }

    case SyncState::FORGET_PROMPT: {
      drawClippedText(renderer, FONT_UI, 20, 80, "Saved password failed", sw - 40, tc);
      drawClippedText(renderer, FONT_SMALL, 20, 120, "Forget saved password?", 0, tc, EpdFontFamily::BOLD);
      drawClippedText(renderer, FONT_SMALL, 20, 145, "Enter/Up: Yes   Down/Esc: No", 0, tc);
      break;
    }
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

// ---------------------------------------------------------------------------
// MicroBASIC SCREEN 1 — 48x15 monospace grid, no interpreter yet, just the
// character surface. Forces landscape-CCW (native panel orientation, no
// rotation transform) regardless of the user's own orientation setting —
// main.cpp saves/restores that around entry/exit. See MicroBASIC repo's
// docs/DEVELOPMENT_LOG.md for the full SCREEN mode spec this implements.
// ---------------------------------------------------------------------------
static void utf8Encode(uint32_t cp, char out[5]) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    out[1] = '\0';
  } else if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    out[2] = '\0';
  } else if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    out[3] = '\0';
  } else {
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    out[4] = '\0';
  }
}

void drawScreenEditor(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();

  const int cursorRow = screenEditorGetCursorRow();
  const int cursorCol = screenEditorGetCursorCol();
  // Hidden while the interpreter has control: this screen is redrawn during
  // a RUN (byield flushes it), and a cursor block parked beside whatever the
  // program printed last is not a cursor, it is a smudge on the picture.
  const bool showCursor = !tbIsRunning();
  const int cols = screenEditorCols();
  const int rows = screenEditorRows();
  const int cellW = screenEditorCellW();
  const int cellH = screenEditorCellH();
  const int marginX = screenEditorMarginX();
  const int fontId = screenEditorFontId();

  for (int row = 0; row < rows; row++) {
    const int y = row * cellH;
    for (int col = 0; col < cols; col++) {
      const int x = marginX + col * cellW;
      const bool isCursor = showCursor && (row == cursorRow && col == cursorCol);
      if (isCursor) {
        renderer.fillRect(x, y, cellW, cellH, true);
      }
      char utf8[5];
      utf8Encode(screenEditorGetCell(row, col), utf8);
      renderer.drawText(fontId, x, y, utf8, !isCursor);
    }
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

// --- VC: the full-screen program picker (see vc_browser.h) -----------------
// Everything here is laid out from the active SCREEN mode's own metrics, so
// it reflows across SCREEN 0-3 with no per-mode special cases. Framing is
// done with inverse-video bars rather than line-drawing characters, which
// these fonts don't carry (ASCII + Latin-1 only).

// Draws `text` at (row, col) across exactly `width` cells, padding with
// spaces so an inverse bar stays solid all the way to its end.
static void drawCellRun(GfxRenderer& renderer, int fontId, int row, int col, int width,
                        const char* text, int cellW, int cellH, int marginX, bool inverse) {
  const int y = row * cellH;
  const int len = (int)strlen(text);
  for (int i = 0; i < width; i++) {
    const int x = marginX + (col + i) * cellW;
    if (inverse) renderer.fillRect(x, y, cellW, cellH, true);
    const char c = (i < len) ? text[i] : ' ';
    if (c != ' ') {
      const char s[2] = {c, '\0'};
      renderer.drawText(fontId, x, y, s, !inverse);
    }
  }
}

void drawVcBrowser(GfxRenderer& renderer, HalGPIO& gpio) {
  (void)gpio;
  renderer.clearScreen();

  const int cols = screenEditorCols();
  const int rows = screenEditorRows();
  const int cellW = screenEditorCellW();
  const int cellH = screenEditorCellH();
  const int marginX = screenEditorMarginX();
  const int fontId = screenEditorFontId();

  const int columns = vcColumns();
  const int rowsPerCol = vcRowsPerColumn();
  const int colWidth = vcColumnWidth();
  const int top = vcScrollTop();
  const int count = vcFileCount();
  const int sel = vcSelectedIndex();

  char buf[128];

  drawCellRun(renderer, fontId, 0, 0, cols, " MicroBASIC - Programs", cellW, cellH, marginX, true);

  // Column-major: entries run down a column, then continue in the next one.
  for (int i = 0; i < columns * rowsPerCol; i++) {
    const int index = top + i;
    if (index >= count) break;
    const int c = i / rowsPerCol;
    const int r = i % rowsPerCol;
    const bool isSel = (index == sel);

    snprintf(buf, sizeof(buf), " %.*s", colWidth - 2, vcFileName(index));
    drawCellRun(renderer, fontId, 1 + r, c * colWidth, colWidth, buf, cellW, cellH, marginX, isSel);
  }

  if (count == 0) {
    drawCellRun(renderer, fontId, 2, 0, cols, "  (no programs saved yet)", cellW, cellH, marginX,
                false);
  }

  if (count > 0) {
    snprintf(buf, sizeof(buf), " %lu B  %s", vcFileSize(sel), vcStatusText());
  } else {
    snprintf(buf, sizeof(buf), " %s", vcStatusText());
  }
  drawCellRun(renderer, fontId, rows - 1, 0, cols, buf, cellW, cellH, marginX, true);

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

