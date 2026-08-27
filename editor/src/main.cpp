// MicroWriter-BASIC-PaperS3 -- the firmware's entry point, for both machines.
//
// Boots straight into the screen editor and BASIC interpreter. On top of that
// there is a status bar (READER, EDITOR, SYNC, KBD, BLE), each button with a
// typed equivalent so nothing needs touch, and three full-screen states that
// take over the panel while they are up: the WiFi transfer wizard
// (wifi_sync.cpp), the file browser and prose editor (file_browser.cpp), and
// the READER dual-boot confirmation (ota_apps.cpp).
//
// This file owns the drawing and the panel geometry. The modules above own
// their own state and key handling and know nothing about the renderer -- the
// same split screen_editor.cpp already used, and the reason the X4's
// ui_renderer.cpp never had to come across with text_editor.cpp and
// file_manager.cpp.
//
// Still in port-staging/ and not ported: vc_browser.cpp.

#include <Arduino.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalDisplay.h>
#include <InputManager.h>
#include <SDCardManager.h>
#include <builtinFonts/notosans_12_regular.h>
#include <builtinFonts/notosans_14_regular.h>
#include <builtinFonts/notosans_16_bold.h>
#include <builtinFonts/ubuntu_10_regular.h>
#include <builtinFonts/unscii_30x60.h>
#include <builtinFonts/unscii_20x40.h>
#include <builtinFonts/unscii_15x30.h>
#include <builtinFonts/unscii_12x24.h>

#include "config.h"
#include "input_handler.h"
#include "osk.h"
#include "sd_datetime.h"
#if !MICROWRITER
#include "screen_editor.h"
#endif
#if !MICROWRITER
#include "tb_bridge.h"
#endif
#include "file_browser.h"
#include "text_editor.h"
#include <Utf8.h>
#include "ota_apps.h"
#include "wifi_sync.h"
#include <BleKeyboardHost.h>
#include <UsbHidKeyboardHost.h>

// Logging.h (pulled in transitively by the hal headers) redefines Serial as a
// proxy whose methods this build does not link. The bring-up wants the plain
// Arduino Serial, so put it back.
// With ARDUINO_USB_CDC_ON_BOOT the Arduino core itself defines Serial as
// USBSerial, and Logging.h has already replaced that define with its own proxy.
// Point it back at the real USB CDC object.
#ifdef Serial
#undef Serial
#endif
#define Serial logSerial  // Logging.h's HWCDC& bound to the real Serial

#include <cstdio>
#include <cstring>

// What this machine calls itself, at runtime: the BLE host name a keyboard
// sees, and the serial banner. Follows the build, so a MicroWriter does not
// announce itself as MicroBASIC. Bonds are keyed by the peer's address, not by
// this, so changing it does not cost a paired keyboard.
#if MICROWRITER
#define MACHINE_NAME "MicroWriter-PaperS3"
#else
#define MACHINE_NAME "MicroBASIC-PaperS3"
#endif

// --- panel geometry ----------------------------------------------------
static constexpr int PANEL_W = 960;  // logical, landscape (native panel orientation)
static constexpr int PANEL_H = 540;

// On-screen keyboard overlay: covers the bottom of the screen when shown,
// full panel width, independent of whatever SCREEN mode/cell size the
// terminal is currently using -- its own geometry never changes with that.
// 360px gives osk.cpp's 6 rows a 60px row height -- back to what it was
// before the Esc row was added (5 rows at 60px = 300px); at 50px (300/6)
// typing got noticeably harder on the physical panel, so this stays
// comfortable at the cost of a bit more of the terminal being covered
// while the keyboard is up.
static constexpr int OSK_H = 360;
static constexpr int OSK_Y = PANEL_H - OSK_H;  // 180
static constexpr int OSK_X = 0;
static constexpr int OSK_W = PANEL_W;

// Top status/function bar: the full panel width, reserved space rather than
// an overlay -- screen_editor.cpp's MODES table accounts for it (one fewer
// terminal row and a taller marginY in every SCREEN mode; see its own
// comment), so terminal content never draws underneath it. Buttons are laid
// out right-to-left, most-used closest to the corner: BLE, KBD, SYNC and
// READER are live; EDITOR is a reserved slot -- drawn, tap-recognized, but not
// wired to anything yet, since the prose editor isn't ported (see the README's
// "Sibling project" section). The title block takes whatever is left, so it
// shrinks on its own as buttons are added rather than needing to be
// recalculated by hand -- see layoutStatusBar().
static constexpr int STATUS_BAR_H = 30;
static constexpr int STATUS_BAR_Y = 0;
// The width the short-labelled buttons have always had, and the reference for
// everything else in the bar: SYNC/KBD/BLE keep it exactly.
static constexpr int STATUS_BTN_W = 90;

// Buttons are sized at boot, not at compile time, because two of the labels
// stopped fitting. READER and EDITOR are six letters where SYNC is four, so at
// a fixed 90px their text ran nearly into the border while SYNC sat in
// comfortable space -- same rectangle, visibly different padding, which is
// what made the pair look cramped on the panel. Now every button gets the
// SAME internal padding, taken from what SYNC has at 90px, and the ones with
// longer labels simply come out wider. That also means a future button, or a
// relabelled one, lays itself out instead of needing a new magic number.
//
// Laid out right to left and flush, no gaps: adjacent boxes end up 4px apart
// (each draws inset by 2), which is the spacing the right-hand three have
// always had. The title block is flush with the leftmost button for the same
// reason, so every seam in the bar matches.
static int g_bleX = PANEL_W - STATUS_BTN_W, g_bleW = STATUS_BTN_W;
static int g_kbdX = 0, g_kbdW = STATUS_BTN_W;
static int g_syncX = 0, g_syncW = STATUS_BTN_W;
static int g_editorX = 0, g_editorW = STATUS_BTN_W;
static int g_readerX = 0, g_readerW = STATUS_BTN_W;
static int g_titleW = 0;

static constexpr int STATUS_TITLE_X = 0;
static constexpr int STATUS_TITLE_Y = STATUS_BAR_Y;
static constexpr int STATUS_TITLE_H = STATUS_BAR_H;



// `display` is a global owned by the hal (declared extern in HalDisplay.h,
// defined in HalDisplay.cpp) -- do not shadow it with a local instance.
static GfxRenderer renderer(display);
static InputManager input;
// The built-in UI fonts carried over from the reader are DEFLATE-compressed
// (they ship an EpdFontGroup table), so the renderer needs a decompressor or
// every glyph logs "Compressed font but no FontDecompressor set" and draws
// nothing. MicroBASIC's own SCREEN fonts will be uncompressed, but the UI
// chrome uses these.
static FontDecompressor fontDecompressor;
static FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());

static EpdFont uiRegularFont(&notosans_12_regular);
static EpdFont bodyRegularFont(&notosans_14_regular);
static EpdFont titleBoldFont(&notosans_16_bold);
static EpdFont smallRegularFont(&ubuntu_10_regular);
static EpdFontFamily uiFamily(&uiRegularFont);
static EpdFontFamily bodyFamily(&bodyRegularFont);
static EpdFontFamily titleFamily(&titleBoldFont);
static EpdFontFamily smallFamily(&smallRegularFont);

// MicroBASIC's own SCREEN fonts -- uncompressed, no FontDecompressor needed.
// All four are registered (not just the default) since "SCREEN n" can
// switch modes at runtime.
static EpdFont screenMono0Font(&unscii_30x60);
static EpdFont screenMono1Font(&unscii_20x40);
static EpdFont screenMono2Font(&unscii_15x30);
static EpdFont screenMono3Font(&unscii_12x24);
static EpdFontFamily screenMono0Family(&screenMono0Font);
static EpdFontFamily screenMono1Family(&screenMono1Font);
static EpdFontFamily screenMono2Family(&screenMono2Font);
static EpdFontFamily screenMono3Family(&screenMono3Font);

static char sdLine[96] = "SD: not probed";
static bool firstPaintDone = false;
static bool g_oskVisible = false;

// Call once, after the UI font is registered -- it measures text.
static void layoutStatusBar() {
  const int syncPad = (STATUS_BTN_W - renderer.getTextWidth(FONT_UI, "SYNC")) / 2;
  auto widthFor = [&](const char* label) {
    const int w = renderer.getTextWidth(FONT_UI, label) + 2 * syncPad;
    return w < STATUS_BTN_W ? STATUS_BTN_W : w;  // never narrower than the others
  };

  g_bleW = STATUS_BTN_W;
  g_kbdW = STATUS_BTN_W;
  g_syncW = STATUS_BTN_W;
#if MICROWRITER
  g_editorW = 0;  // no EDITOR button: the browser is this machine's home screen
#else
  g_editorW = widthFor("EDITOR");
#endif
  g_readerW = widthFor("READER");

  g_bleX = PANEL_W - g_bleW;
  g_kbdX = g_bleX - g_kbdW;
  g_syncX = g_kbdX - g_syncW;
  g_editorX = g_syncX - g_editorW;
  g_readerX = g_editorX - g_readerW;
  g_titleW = g_readerX - STATUS_TITLE_X;

  Serial.printf("[ui] status bar: pad=%d reader=%d editor=%d title=%d\n", syncPad, g_readerW, g_editorW, g_titleW);
}

// Defined in input_handler.cpp; also written by tb_bridge.cpp/tb_runtime.cpp
// whenever the interpreter changes what's on screen.
extern bool screenDirty;

static void probeSdCard() {
  if (!SdMan.begin()) {
    snprintf(sdLine, sizeof(sdLine), "SD: begin() FAILED");
    return;
  }
  int files = 0;
  FsFile dir = SdMan.open("/");
  if (dir) {
    FsFile entry;
    while (entry.openNext(&dir, O_RDONLY)) {
      files++;
      entry.close();
    }
    dir.close();
  }
  snprintf(sdLine, sizeof(sdLine), "SD: mounted, %d entries in /", files);
}

// Declared extern by tb_bridge.cpp: on the X4 this rearms the d-pad's
// edge-detection after the interpreter returns control. This device has no
// physical button to rearm, so it's empty on purpose. pumpPhysicalButtonsForProgram()
// (also declared extern, by tb_runtime.cpp) is NOT a no-op here, despite the
// name -- see its real definition below handleTouchTap()/screenEditorFlushDisplay(),
// which it needs.
void physicalButtonsRearm() {}

// osk.cpp's callback: exactly the (HID keycode, HID modifiers) pair
// enqueueKeyEvent() expects, plus the `pressed=true` a discrete tap always
// is -- osk.cpp only ever reports a completed tap, never separate down/up
// edges, so every call here is a press. Matches how ble_keyboard.cpp calls
// enqueueKeyEvent() for a newly-pressed HID key.
static void onOskKey(uint8_t hidCode, uint8_t modifiers) {
  enqueueKeyEvent(hidCode, modifiers, true);
}

// screenEditorGetCell() returns a raw Unicode codepoint per cell (dead key
// composition inserts the composed codepoint directly -- see
// input_handler.cpp's handleScreenEditorKey()), not a Latin-1 byte string,
// so this is a real per-codepoint UTF-8 encoder, not the narrower Latin-1
// case the on-screen keyboard's own earlier typing demo used. Every
// codepoint this project's dead keys or keyboard can actually produce stays
// within the Latin-1 Supplement range (<=0xFF), so 2 bytes/column is the
// real worst case, matching the buffer size callers use.
static int codepointToUtf8(uint32_t cp, char* out) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  // Not reachable with this project's current input sources, but handled
  // rather than truncated silently if that ever changes.
  out[0] = (char)(0xE0 | (cp >> 12));
  out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[2] = (char)(0x80 | (cp & 0x3F));
  return 3;
}

#if !MICROWRITER
static void drawTerminalContent() {
  const int cols = screenEditorCols();
  const int rows = screenEditorRows();
  const int cellW = screenEditorCellW();
  const int cellH = screenEditorCellH();
  const int marginY = screenEditorMarginY();
  const int fontId = screenEditorFontId();

  char utf8Row[SCREEN_EDITOR_MAX_COLS * 3 + 1];
  for (int r = 0; r < rows; r++) {
    int o = 0;
    for (int c = 0; c < cols; c++) {
      o += codepointToUtf8(screenEditorGetCell(r, c), utf8Row + o);
    }
    utf8Row[o] = '\0';
    renderer.drawText(fontId, 0, marginY + r * cellH, utf8Row);
  }

  // Cursor: a solid block at the next insertion point. Hidden while the
  // interpreter is running -- tb_bridge.h's own doc comment explains why: a
  // cursor means "waiting for you to type", and nothing is, and on a
  // program that repaints cells in place it otherwise reads as a block
  // stuck to the sprite.
  if (!tbIsRunning()) {
    const int cx = screenEditorGetCursorCol() * cellW;
    const int cy = marginY + screenEditorGetCursorRow() * cellH;
    renderer.fillRect(cx, cy, cellW, cellH, true);
  }
}
#endif

static void drawToggleButton() {
  const int inset = 2;
  renderer.drawRect(g_kbdX + inset, STATUS_BAR_Y + inset, g_kbdW - 2 * inset, STATUS_BAR_H - 2 * inset,
                     true);
  const char* label = g_oskVisible ? "Hide" : "KBD";
  const int tw = renderer.getTextWidth(FONT_UI, label);
  const int tx = g_kbdX + (g_kbdW - tw) / 2;
  const int ty = STATUS_BAR_Y + (STATUS_BAR_H - renderer.getLineHeight(FONT_UI)) / 2;
  renderer.drawText(FONT_UI, tx, ty, label);
}

// Status + manual-pair button, immediately left of the panel's right edge
// in the status bar's row (see BLE_TOGGLE_* above). Filled solid while a
// BLE keyboard is connected (matches osk.cpp's own "armed" convention for
// Shift/Ctrl/Alt), outlined otherwise -- so the connection state is visible
// at a glance without needing to read anything. Tapping it always fires
// forceBlePairingNow() (defined near bleKbdAutoPair() below, forward-
// declared here since this file's touch handling is laid out above its BLE
// pairing logic).
static void forceBlePairingNow();

static void drawBleButton() {
  const int inset = 2;
  const bool connected = BleHid.isConnected();
  if (connected) {
    renderer.fillRect(g_bleX + inset, STATUS_BAR_Y + inset, g_bleW - 2 * inset,
                       STATUS_BAR_H - 2 * inset, true);
  } else {
    renderer.drawRect(g_bleX + inset, STATUS_BAR_Y + inset, g_bleW - 2 * inset,
                       STATUS_BAR_H - 2 * inset, true);
  }
  const char* label = "BLE";
  const int tw = renderer.getTextWidth(FONT_UI, label);
  const int tx = g_bleX + (g_bleW - tw) / 2;
  const int ty = STATUS_BAR_Y + (STATUS_BAR_H - renderer.getLineHeight(FONT_UI)) / 2;
  renderer.drawText(FONT_UI, tx, ty, label, !connected);
}

// MENU/EDITOR/SYNC: reserved status-bar slots for systems that aren't
// ported yet (see the README's "Sibling project" section). Drawn outlined,
// like an unarmed KBD/BLE button, and tap-recognized (handleTouchTap()
// swallows a tap here rather than letting it fall through) so the space
// visibly belongs to a future button instead of silently doing nothing.
static void drawPlaceholderButton(int x, int y, int w, int h, const char* label) {
  const int inset = 2;
  renderer.drawRect(x + inset, y + inset, w - 2 * inset, h - 2 * inset, true);
  const int tw = renderer.getTextWidth(FONT_UI, label);
  const int tx = x + (w - tw) / 2;
  const int ty = y + (h - renderer.getLineHeight(FONT_UI)) / 2;
  renderer.drawText(FONT_UI, tx, ty, label);
}

// SYNC: filled while the WiFi setup screen is up (matches BLE's own
// "armed" convention), outlined otherwise. Tapping it while active is a
// no-op (wifiSyncStart() already guards on isWifiSyncActive()) -- Escape,
// reachable from every state syncHandleKey() has, is the way out.
static void drawSyncButton() {
  const int inset = 2;
  const bool active = isWifiSyncActive();
  if (active) {
    renderer.fillRect(g_syncX + inset, STATUS_BAR_Y + inset, g_syncW - 2 * inset,
                       STATUS_BAR_H - 2 * inset, true);
  } else {
    renderer.drawRect(g_syncX + inset, STATUS_BAR_Y + inset, g_syncW - 2 * inset,
                       STATUS_BAR_H - 2 * inset, true);
  }
  const char* label = "SYNC";
  const int tw = renderer.getTextWidth(FONT_UI, label);
  const int tx = g_syncX + (g_syncW - tw) / 2;
  const int ty = STATUS_BAR_Y + (STATUS_BAR_H - renderer.getLineHeight(FONT_UI)) / 2;
  renderer.drawText(FONT_UI, tx, ty, label, !active);
}

// The sibling app to hand control back to, resolved once at boot.
// partitionSubtype 0 means "no sibling found", which is the normal state on a
// unit flashed with this firmware alone -- the READER button then says so
// instead of doing nothing silently.
static int g_readerSubtype = 0;
static char g_readerName[32] = "READER";

// Set while the confirmation for it is on screen; see drawReaderConfirm().
static bool g_readerConfirm = false;

// READER: filled while its confirmation is up (matching BLE/SYNC's own "armed"
// convention), outlined otherwise.
static void drawReaderButton() {
  const int inset = 2;
  if (g_readerConfirm) {
    renderer.fillRect(g_readerX + inset, STATUS_BAR_Y + inset, g_readerW - 2 * inset,
                       STATUS_BAR_H - 2 * inset, true);
  } else {
    renderer.drawRect(g_readerX + inset, STATUS_BAR_Y + inset, g_readerW - 2 * inset,
                       STATUS_BAR_H - 2 * inset, true);
  }
  const char* label = "READER";
  const int tw = renderer.getTextWidth(FONT_UI, label);
  const int tx = g_readerX + (g_readerW - tw) / 2;
  const int ty = STATUS_BAR_Y + (STATUS_BAR_H - renderer.getLineHeight(FONT_UI)) / 2;
  renderer.drawText(FONT_UI, tx, ty, label, !g_readerConfirm);
}

// Everything in the status bar: the reserved placeholder button, the
// live ones, and the outlined title area filling whatever's left of the
// bar (g_titleW already accounts for how many buttons exist).
static void drawStatusBar() {
  drawReaderButton();
#if !MICROWRITER
  drawPlaceholderButton(g_editorX, STATUS_BAR_Y, g_editorW, STATUS_BAR_H, "EDITOR");
#endif
  drawSyncButton();
  drawToggleButton();
  drawBleButton();

  const int inset = 2;
  renderer.drawRect(STATUS_TITLE_X + inset, STATUS_TITLE_Y + inset, g_titleW - 2 * inset,
                     STATUS_TITLE_H - 2 * inset, true);
  #if MICROWRITER
  const char* title = "FSP MicroWriter Paper S3 v0.5";
#else
  const char* title = "FSP MicroBASIC Paper S3 v0.5";
#endif
  // Left-aligned, not centered like the buttons -- a title reads as a label
  // for the bar, not another button, so it gets its own left margin instead
  // of floating in the middle of a box that's going to keep shrinking as
  // more buttons claim the space to its right.
  constexpr int kTitleLeftMargin = 8;
  const int tx = STATUS_TITLE_X + inset + kTitleLeftMargin;
  const int ty = STATUS_TITLE_Y + (STATUS_TITLE_H - renderer.getLineHeight(FONT_UI)) / 2;
  renderer.drawText(FONT_UI, tx, ty, title);
}

// WiFi setup screen: the band between the status bar and the on-screen
// keyboard (forced visible for the whole session -- see the SYNC tap
// handler above). Network selection is an inverted highlight bar moved by
// Up/Down -- reachable from a physical/BLE keyboard's arrow keys or the
// on-screen keyboard's, with no per-row touch targets to hit-test, per the
// design discussion this was built from. Password characters are shown as
// dots, matching how every other WiFi password entry UI does it.
// The band a full-screen UI (the WiFi wizard, the EDITOR browser) draws into:
// everything between the status bar and whatever is below it. Its HEIGHT is
// not fixed, because the on-screen keyboard can be toggled away while one of
// those screens is up -- when it is hidden there are another 360px to use, and
// a list that stayed at the with-keyboard height would waste them.
static constexpr int WIFI_UI_X = 0;
static constexpr int WIFI_UI_Y = STATUS_BAR_Y + STATUS_BAR_H;
static constexpr int WIFI_UI_W = PANEL_W;
static int contentBandH() { return (g_oskVisible ? OSK_Y : PANEL_H) - WIFI_UI_Y; }

static void drawWifiCentered(const char* line1, const char* line2 = nullptr) {
  const int lh = renderer.getLineHeight(FONT_UI);
  const int ty1 = WIFI_UI_Y + contentBandH() / 2 - (line2 ? lh : lh / 2);
  const int tw1 = renderer.getTextWidth(FONT_UI, line1);
  renderer.drawText(FONT_UI, WIFI_UI_X + (WIFI_UI_W - tw1) / 2, ty1, line1);
  if (line2) {
    const int tw2 = renderer.getTextWidth(FONT_UI, line2);
    renderer.drawText(FONT_UI, WIFI_UI_X + (WIFI_UI_W - tw2) / 2, ty1 + lh, line2);
  }
}

static void drawNetworkList() {
  const int rowH = renderer.getLineHeight(FONT_SMALL) + 4;
  const int visibleRows = contentBandH() / rowH;
  const int count = getNetworkCount();
  const int sel = getSelectedNetwork();

  if (count == 0) {
    const char* msg = getSyncStatusText();
    drawWifiCentered(msg[0] ? msg : "No networks found", "Esc to cancel");
    return;
  }

  // Keep the selection inside the visible window, scrolling the minimum
  // amount needed rather than always centering it.
  static int scrollTop = 0;
  if (sel < scrollTop) scrollTop = sel;
  if (sel >= scrollTop + visibleRows) scrollTop = sel - visibleRows + 1;
  if (scrollTop > count - visibleRows) scrollTop = count > visibleRows ? count - visibleRows : 0;
  if (scrollTop < 0) scrollTop = 0;

  for (int row = 0; row < visibleRows && scrollTop + row < count; row++) {
    const int i = scrollTop + row;
    const int y = WIFI_UI_Y + row * rowH;
    const bool selected = (i == sel);
    if (selected) renderer.fillRect(WIFI_UI_X, y, WIFI_UI_W, rowH, true);

    char line[80];
    snprintf(line, sizeof(line), "%s%s%s", isNetworkSaved(i) ? "[saved] " : "",
             getNetworkSSID(i), isNetworkEncrypted(i) ? "  (locked)" : "");
    const int ty = y + (rowH - renderer.getLineHeight(FONT_SMALL)) / 2;
    renderer.drawText(FONT_SMALL, 8, ty, line, !selected);
  }
}

static void drawPasswordEntry() {
  char header[48];
  snprintf(header, sizeof(header), "Password for %s:", getNetworkSSID(getSelectedNetwork()));
  const int lh = renderer.getLineHeight(FONT_UI);
  renderer.drawText(FONT_UI, 8, WIFI_UI_Y + 8, header);

  char dots[MAX_FILENAME_LEN];
  const int n = getPasswordLen();
  const int shown = n < (int)sizeof(dots) - 1 ? n : (int)sizeof(dots) - 1;
  for (int i = 0; i < shown; i++) dots[i] = '*';
  dots[shown] = '\0';
  renderer.drawText(FONT_UI, 8, WIFI_UI_Y + 8 + lh + 8, dots[0] ? dots : "(type on the keyboard below)");
}

// Per-codepoint ADVANCE for text_editor's wrap pass. That module has no font
// dependency of its own by design, so it asks the caller, which does.
//
// Measuring one character with getTextWidth() does NOT give this. That returns
// the ink bounding box (maxX - minX in EpdFont::getTextDimensions), which for a
// single glyph is narrower than its advance by the side bearings -- and for a
// space, which has no ink at all, is essentially zero. Summing those
// under-counts badly on prose, so a line overran the panel by a wide margin
// before the wrap budget was reached.
//
// width("cc") - width("c") is the advance exactly: the second copy starts one
// advance further along and contributes the same ink and bearings the first
// did, so everything but the advance cancels. It gives the right answer for a
// space too.
//
// Cached because text_editor re-wraps the whole buffer on every keystroke, and
// this would otherwise be two text measurements per character per keystroke.
static int utf8Encode(uint32_t cp, char* out) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

static int measureAdvance(uint32_t cp) {
  char one[5] = {0}, two[9] = {0};
  const int n = utf8Encode(cp, one);
  memcpy(two, one, n);
  memcpy(two + n, one, n);
  return renderer.getTextWidth(FONT_BODY, two) - renderer.getTextWidth(FONT_BODY, one);
}

static int editorGlyphWidth(uint32_t cp) {
  // Printable ASCII is almost everything typed, and is measured once.
  static int cache[95] = {0};  // 0x20..0x7E
  if (cp >= 0x20 && cp <= 0x7E) {
    int& slot = cache[cp - 0x20];
    if (slot == 0) slot = measureAdvance(cp);
    return slot;
  }
  return measureAdvance(cp);
}

// Width of a byte range, summed the same way the wrap pass counts and the same
// way drawText actually advances. Not getTextWidth(): that is the ink box, so a
// prefix ending in a space would measure short and put the caret, or the edge
// of a selection highlight, in the wrong place.
static int advanceWidth(const char* s, int nbytes) {
  int w = 0;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
  const unsigned char* end = p + nbytes;
  while (p < end) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;
    w += editorGlyphWidth(cp);
  }
  return w;
}

// The prose editor. Draws the wrapped lines text_editor.cpp computed, from
// its viewport, plus a caret. Geometry is pushed in every frame because the
// band changes height when the on-screen keyboard is toggled.
static void drawEditorUi() {
  const int lh = renderer.getLineHeight(FONT_BODY);
  const int margin = 8;
  const int bandH = contentBandH();
  const int visible = bandH / lh;

  editorSetMaxLineWidthPx(WIFI_UI_W - 2 * margin);
  editorSetVisibleLines(visible);
  editorSetPageJumpLines(visible > 1 ? visible - 1 : 1);
  editorRecalculateLines();

  const char* buf = editorGetBuffer();
  const int lineCount = editorGetLineCount();
  const int top = editorGetViewportStart();
  const int cursorLine = editorGetCursorLine();

  for (int row = 0; row < visible && top + row < lineCount; row++) {
    const int i = top + row;
    const int start = editorGetLinePosition(i);
    const int end = (i + 1 < lineCount) ? editorGetLinePosition(i + 1) : (int)editorGetLength();

    char line[256];
    int n = end - start;
    if (n < 0) n = 0;
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
    memcpy(line, buf + start, n);
    // Trim the newline the wrap kept, so it is not drawn as a glyph.
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
    line[n] = '\0';

    const int y = WIFI_UI_Y + row * lh;
    renderer.drawText(FONT_BODY, margin, y, line);

    // Selection: fill the selected span black and redraw just that text in
    // white on top, so it reads inverted like every other list on the device.
    if (editorHasSelection()) {
      const int selA = editorGetSelectionStart();
      const int selB = editorGetSelectionEnd();
      const int a = (selA > start) ? selA : start;          // clip to this line
      const int b = (selB < start + n) ? selB : start + n;
      if (b > a) {
        const int preBytes = a - start;
        const int selBytes = b - a;
        const int x1 = margin + advanceWidth(line, preBytes);
        const int w = advanceWidth(line + preBytes, selBytes);
        renderer.fillRect(x1, y, w, lh, true);

        char mid[256];
        int mn = selBytes;
        if (mn > (int)sizeof(mid) - 1) mn = (int)sizeof(mid) - 1;
        memcpy(mid, line + preBytes, mn);
        mid[mn] = '\0';
        renderer.drawText(FONT_BODY, x1, y, mid, false);
      }
    }

    // Caret: a vertical bar at the cursor's column within its own line.
    if (i == cursorLine) {
      int c = editorGetCursorCol();
      if (c > n) c = n;
      const int cx = margin + advanceWidth(line, c);
      renderer.fillRect(cx, y, 2, lh, true);
    }
  }
}

// Naming a new file, or retitling an open one. A new file has no name until
// this is confirmed, which is why it comes before the editor rather than
// after -- see file_browser.h.
static void drawTitleUi() {
  const int lh = renderer.getLineHeight(FONT_UI);
  renderer.drawText(FONT_UI, 8, WIFI_UI_Y + 8, "Title:");

  char shown[MAX_TITLE_LEN + 2];
  snprintf(shown, sizeof(shown), "%s_", browserTitleBuffer());
  renderer.drawText(FONT_UI, 8, WIFI_UI_Y + 8 + lh + 8, shown);

  renderer.drawText(FONT_SMALL, 8, WIFI_UI_Y + 8 + 2 * (lh + 8), "Enter to confirm, Esc to cancel");
}

// EDITOR screen. Same band and same inverted-highlight-bar idiom as the WiFi
// network list, so the two navigate identically -- see file_browser.h.
static void drawBrowserUi() {
  if (getBrowserState() == BrowserState::EDIT) {
    drawEditorUi();
    return;
  }
  if (getBrowserState() == BrowserState::TITLE) {
    drawTitleUi();
    return;
  }

  const int rowH = renderer.getLineHeight(FONT_SMALL) + 4;
  const int visibleRows = contentBandH() / rowH;
  const bool menu = getBrowserState() == BrowserState::MENU;
  const int count = menu ? BROWSER_MENU_COUNT : getFileCount();
  const int sel = getBrowserSelection();

  const char* status = browserStatusText();
  if (count == 0) {
    drawWifiCentered(status[0] ? status : "Nothing here", "Esc to go back");
    return;
  }

  // Same minimal-scroll window the network list uses.
  static int scrollTop = 0;
  if (sel < scrollTop) scrollTop = sel;
  if (sel >= scrollTop + visibleRows) scrollTop = sel - visibleRows + 1;
  if (scrollTop > count - visibleRows) scrollTop = count > visibleRows ? count - visibleRows : 0;
  if (scrollTop < 0) scrollTop = 0;

  for (int row = 0; row < visibleRows && scrollTop + row < count; row++) {
    const int i = scrollTop + row;
    const int y = WIFI_UI_Y + row * rowH;
    const bool selected = (i == sel);
    if (selected) renderer.fillRect(WIFI_UI_X, y, WIFI_UI_W, rowH, true);

    const char* label = menu ? browserMenuLabel(i) : getFileList()[i].title;
    const int ty = y + (rowH - renderer.getLineHeight(FONT_SMALL)) / 2;
    renderer.drawText(FONT_SMALL, 8, ty, label, !selected);
  }

  // Status line goes in the last visible row rather than over a list entry.
  if (status[0]) {
    const int y = WIFI_UI_Y + contentBandH() - renderer.getLineHeight(FONT_SMALL);
    renderer.drawText(FONT_SMALL, 8, y, status);
  }
}

static void drawWifiUi() {
  switch (getSyncState()) {
    case SyncState::SCANNING:
      drawWifiCentered("Scanning for networks...");
      break;
    case SyncState::NETWORK_LIST:
      drawNetworkList();
      break;
    case SyncState::PASSWORD_ENTRY:
      drawPasswordEntry();
      break;
    case SyncState::CONNECTING:
      drawWifiCentered(getSyncStatusText(), "Esc to cancel");
      break;
    case SyncState::SYNCING: {
      char line2[64];
      snprintf(line2, sizeof(line2), "Sent: %d  Received: %d  %s", getSyncFilesSent(),
               getSyncFilesReceived(), isPcConnected() ? "(connected)" : "");
      drawWifiCentered(getSyncStatusText(), line2);
      break;
    }
    case SyncState::DONE:
      drawWifiCentered(getSyncStatusText(), "Returning...");
      break;
    case SyncState::CONNECT_FAILED:
      drawWifiCentered(getSyncStatusText(), "Enter to retry, Esc to cancel");
      break;
    case SyncState::SAVE_PROMPT:
      drawWifiCentered("Save this password?", "Up = Yes    Down = No");
      break;
    case SyncState::FORGET_PROMPT:
      drawWifiCentered("Forget the saved password?", "Up = Yes    Down = No");
      break;
  }
}

static void drawScreen();  // defined below; the confirm handler repaints via it

// Tell the user something, wherever they are looking. MicroBASIC has a
// terminal and everything else prints into it; MicroWriter has no terminal at
// all, so the same message goes to the browser's status line -- the screen
// that build is always on.
static void notify(const char* text) {
#if MICROWRITER
  browserSetStatus(text);
#else
  screenEditorTermPrintLine(text);
#endif
}

// Confirmation for the READER switch. Deliberately modal and centered rather
// than a second tap on the button itself: switching reboots into a different
// firmware, so it should take a decision, not a slip. Both buttons are real
// tap targets AND Enter/Esc work, since the on-screen keyboard may well be
// hidden when someone reaches for this.
static constexpr int CONFIRM_W = 560;
static constexpr int CONFIRM_H = 170;
static constexpr int CONFIRM_X = (PANEL_W - CONFIRM_W) / 2;
static constexpr int CONFIRM_Y = STATUS_BAR_Y + STATUS_BAR_H + 40;
static constexpr int CONFIRM_BTN_W = 180;
static constexpr int CONFIRM_BTN_H = 48;
static constexpr int CONFIRM_BTN_Y = CONFIRM_Y + CONFIRM_H - CONFIRM_BTN_H - 20;
static constexpr int CONFIRM_YES_X = CONFIRM_X + 40;
static constexpr int CONFIRM_NO_X = CONFIRM_X + CONFIRM_W - CONFIRM_BTN_W - 40;

static void drawConfirmButton(int x, const char* label) {
  renderer.drawRect(x, CONFIRM_BTN_Y, CONFIRM_BTN_W, CONFIRM_BTN_H, true);
  const int tw = renderer.getTextWidth(FONT_UI, label);
  renderer.drawText(FONT_UI, x + (CONFIRM_BTN_W - tw) / 2,
                    CONFIRM_BTN_Y + (CONFIRM_BTN_H - renderer.getLineHeight(FONT_UI)) / 2, label);
}

static void drawReaderConfirm() {
  renderer.fillRect(CONFIRM_X, CONFIRM_Y, CONFIRM_W, CONFIRM_H, false);
  renderer.drawRect(CONFIRM_X, CONFIRM_Y, CONFIRM_W, CONFIRM_H, true);

  char line[64];
  snprintf(line, sizeof(line), "Restart into %s?", g_readerName);
  const int lh = renderer.getLineHeight(FONT_UI);
  int tw = renderer.getTextWidth(FONT_UI, line);
  renderer.drawText(FONT_UI, CONFIRM_X + (CONFIRM_W - tw) / 2, CONFIRM_Y + 26, line);

  const char* sub = "This reboots the device.";
  tw = renderer.getTextWidth(FONT_SMALL, sub);
  renderer.drawText(FONT_SMALL, CONFIRM_X + (CONFIRM_W - tw) / 2, CONFIRM_Y + 26 + lh + 6, sub);

  drawConfirmButton(CONFIRM_YES_X, "Yes (Enter)");
  drawConfirmButton(CONFIRM_NO_X, "No (Esc)");
}

// Shared by the tap handler and the key handler so both paths behave
// identically -- see handleTouchTap()'s own note on why that matters.
// Both of these must set screenDirty themselves. The modal's key path in
// loop() deliberately bypasses processAllInput(), which is what normally marks
// the screen dirty on a keystroke -- so without this, Esc clears the flag but
// leaves the dialog painted until something else happens to dirty the buffer.
// On e-ink that reads as "Esc did nothing", and worse, the keys after it go to
// the editor behind a dialog that is visually still there. The touch path was
// never affected: handleTouchTap()'s caller sets screenDirty on a true return.
static void readerConfirmAccept() {
  g_readerConfirm = false;
  if (g_readerSubtype == 0) {
    screenDirty = true;
    return;
  }
  notify("Switching...");
  drawScreen();
  switchToOtaApp(g_readerSubtype);  // reboots; only returns if it failed
  notify("Switch failed -- staying here.");
  screenDirty = true;  // the drawScreen() above was the last paint; without
                       // this the failure message would never reach the glass
}

static void readerConfirmCancel() {
  g_readerConfirm = false;
  screenDirty = true;
}

static bool tapInRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

// Declared in input_handler.h -- see its doc comment.
void startEditorFromCommand() {
  if (isBrowserActive()) return;
  // Only when there is no other way to send an arrow key. SYNC forces the
  // keyboard unconditionally because it SUSPENDS BLE partway through, so a
  // BLE-only typist would be stranded mid-flow; nothing here suspends
  // anything, so a connected keyboard is reason enough to leave the screen
  // whole. The KBD button still toggles it either way, and the list grows
  // into the space when it is hidden (see contentBandH()).
  if (!BleHid.isConnected()) g_oskVisible = true;
  browserStart();
}

// Declared in input_handler.h -- see its doc comment.
#if !MICROWRITER
void startVcFromCommand() {
  if (isBrowserActive()) return;
  if (!BleHid.isConnected()) g_oskVisible = true;
  browserStartVc();
}
#endif

// Declared in input_handler.h -- see its doc comment.
void startReaderSwitchFromCommand() {
  if (g_readerSubtype == 0) {
    notify("No sibling app in the other OTA slot.");
    return;
  }
  g_readerConfirm = true;
  screenDirty = true;
}

// Declared in input_handler.h -- see its doc comment.
void startWifiSyncFromCommand() {
  if (isWifiSyncActive()) return;
  // Same rule as the EDITOR screen: only open the keyboard when there is no
  // other way to type. This used to be unconditional, because connecting to
  // WiFi suspended BLE and would have stranded a BLE-only typist mid-flow --
  // that suspension is gone (it was a workaround for a cause that turned out
  // to be a full NVS partition, see docs/DEVELOPMENT_LOG.md), and with it the
  // reason to force the keyboard. A connected keyboard now leaves the whole
  // band to the network list, which grows into it (see contentBandH()).
  if (!BleHid.isConnected()) g_oskVisible = true;
  wifiSyncStart();
}

// Routes one already-logical-coordinate tap to the toggle button or the
// on-screen keyboard. Returns true if something changed and a redraw is
// due. Shared between loop() (the normal case) and
// pumpPhysicalButtonsForProgram() (touch polling during a blocked BASIC
// run, see that function's own comment) -- both need exactly this
// dispatch, and diverging would mean a tap behaving differently depending
// on whether a program happened to be running when it landed.
static bool handleTouchTap(int lx, int ly) {
  // The confirmation is modal: while it is up it swallows every tap, so a
  // stray hit on the status bar underneath can't act behind it.
  if (g_readerConfirm) {
    if (tapInRect(lx, ly, CONFIRM_YES_X, CONFIRM_BTN_Y, CONFIRM_BTN_W, CONFIRM_BTN_H)) {
      readerConfirmAccept();
    } else if (tapInRect(lx, ly, CONFIRM_NO_X, CONFIRM_BTN_Y, CONFIRM_BTN_W, CONFIRM_BTN_H)) {
      readerConfirmCancel();
    }
    return true;
  }
  if (tapInRect(lx, ly, g_kbdX, STATUS_BAR_Y, g_kbdW, STATUS_BAR_H)) {
    g_oskVisible = !g_oskVisible;
    return true;
  }
  if (tapInRect(lx, ly, g_bleX, STATUS_BAR_Y, g_bleW, STATUS_BAR_H)) {
    forceBlePairingNow();
    return true;
  }
  if (tapInRect(lx, ly, g_syncX, STATUS_BAR_Y, g_syncW, STATUS_BAR_H)) {
    startWifiSyncFromCommand();
    return true;
  }
  if (tapInRect(lx, ly, g_readerX, STATUS_BAR_Y, g_readerW, STATUS_BAR_H)) {
    if (g_readerSubtype == 0) {
      notify("No sibling app in the other OTA slot.");
    } else {
      g_readerConfirm = true;
    }
    return true;
  }
#if !MICROWRITER
  if (tapInRect(lx, ly, g_editorX, STATUS_BAR_Y, g_editorW, STATUS_BAR_H)) {
    startEditorFromCommand();
    return true;
  }
#endif
  if (g_oskVisible) {
    // oskHandleTap() bounds-checks against the region passed to oskInit()
    // and returns false outside it; gated on g_oskVisible too so a tap over
    // the (currently hidden) overlay's fixed region doesn't get misread as
    // a key press while real terminal content is shown there. A hit either
    // arms/toggles a modifier (drawn differently, needs a redraw) or calls
    // onOskKey() -> enqueueKeyEvent() -- including Escape or Ctrl+C, which
    // is exactly how a running program gets broken; see
    // pumpPhysicalButtonsForProgram()'s comment for why that needs this
    // same routing to actually be reachable mid-run.
    return oskHandleTap(lx, ly);
  }
  return false;
}

static void drawScreen() {
  renderer.clearScreen();
  if (isWifiSyncActive()) {
    drawWifiUi();
  } else if (isBrowserActive()) {
    drawBrowserUi();
  }
#if !MICROWRITER
  else {
    drawTerminalContent();
  }
#endif
  drawStatusBar();
  if (g_oskVisible) oskDraw();
  if (g_readerConfirm) drawReaderConfirm();  // last: modal, draws over everything

  Serial.printf("[paint] displayBuffer(%s) ...\n", firstPaintDone ? "FAST" : "FULL");
  Serial.flush();
  const uint32_t t0 = millis();
  renderer.displayBuffer(firstPaintDone ? HalDisplay::FAST_REFRESH : HalDisplay::FULL_REFRESH);
  Serial.printf("[paint] returned after %lu ms\n", (unsigned long)(millis() - t0));
  Serial.flush();
  firstPaintDone = true;
}

// Declared in screen_editor.h, defined here per its own doc comment: normal
// screen updates go through loop() below, but a running BASIC program blocks
// loopTask for its whole duration inside the interpreter, so byield() in
// tb_runtime.cpp calls this directly (throttled by wall-clock time) to keep
// a running program's output visible as it goes.
void screenEditorFlushDisplay() { drawScreen(); }

// Declared extern by tb_runtime.cpp, called from byield() at the same
// cadence as its own scheduler yield -- byield()'s own comment: "The d-pad
// only reaches a running program through here: loop() is blocked inside
// the interpreter for the whole run." Same is true here, but there is no
// d-pad -- there is touch, which loop() normally polls, and loop() is
// exactly what's blocked. Without this actually polling touch, NOTHING
// during a run ever calls input.update() or routes a tap anywhere: not
// Escape/Ctrl+C to checkch()'s break check (pumpProgramInput(), reached via
// inputConsumeBreakPending()), not a tap on the "Hide" button. Both read as
// "touch stopped working" and needed a physical reboot before this existed.
//
// screenEditorFlushDisplay() is called directly (not just left to
// byield()'s own throttled call after this returns) when a tap actually
// changed something: byield() gates its own flush on tb_runtime.cpp's
// termDirty, which only outch() sets, so a tap that toggles the keyboard
// without the program having printed anything would otherwise sit invisible
// until the next unrelated redraw.
void pumpPhysicalButtonsForProgram() {
  // A physical USB keyboard lands its reports on its own background task
  // (see UsbHidKeyboardHost.h) regardless of whether loop() is blocked, but
  // that task only fills UsbKbd's own internal queue -- something still has
  // to drain it into enqueueKeyEvent() for checkch()'s break check
  // (inputConsumeBreakPending()) or GET (inputReadProgramKey()) to ever see
  // it. Without this, Ctrl+C/Escape from a physical keyboard would suffer
  // exactly the dead-during-a-run bug touch had before pumpPhysicalButtonsForProgram()
  // grew a body at all.
  uint8_t kbdCode, kbdMods;
  bool kbdPressed;
  while (UsbKbd.popKey(kbdCode, kbdMods, kbdPressed)) {
    enqueueKeyEvent(kbdCode, kbdMods, kbdPressed);
  }

  // Same reasoning as UsbKbd above, for the BLE keyboard: poll() drives its
  // auto-reconnect and held-key aging (see BleKeyboardHost.h), and popKey()
  // needs draining into enqueueKeyEvent() for Ctrl+C break to reach a
  // running program. bleKbdAutoPair() is deliberately NOT called here --
  // first-time scanning is slow and has no reason to run while a program is
  // mid-execution; it only matters before any keyboard has ever been paired.
  BleHid.poll();
  freeink::KeyEvent bleEv;
  while (BleHid.popKey(bleEv)) {
    enqueueKeyEvent(bleEv.keycode, bleEv.mods, bleEv.pressed);
  }

  input.update();
  float nx, ny;
  if (!input.wasTouchTap(nx, ny)) return;
  int lx, ly;
  renderer.tapToLogical(nx, ny, lx, ly);
  if (handleTouchTap(lx, ly)) screenEditorFlushDisplay();
}

// No pairing UI exists yet (menu/settings are still TODO -- see
// input_handler.cpp's own TODOs), so this drives pairing itself, in two
// layers:
//
// 1. No bond yet: scan for kBleScanDurationMs and connect to the first
//    HID-advertising, connectable device seen -- this project's only
//    physical-input use case is a keyboard, so unlike a general-purpose
//    pairing UI there's no need to discriminate keyboard vs. mouse vs.
//    other HID peripherals here.
//
// 2. A bond exists but nothing is connecting: BleKeyboardHost's own poll()
//    already retries a direct connect-by-address to the saved bond every
//    few seconds on its own (see BleKeyboardHost.cpp's kReconnectBackoffMs)
//    -- that's cheap and is given a fair run first. But a direct
//    connect-by-address only succeeds if the peripheral happens to be
//    advertising in the narrow window NimBLE's connect() looks for it in;
//    it does nothing if the keyboard went to sleep and stopped general
//    advertising, or if it needs to be put back into pairing mode (which
//    some keyboards use a *different* advertisement for than their normal
//    "reconnect to bonded host" one). Confirmed on hardware: after the
//    keyboard's own idle timeout dropped the link, waiting for the direct
//    reconnect alone never got it back, even with the keyboard back in
//    pairing mode. So once kBleBondedRetryGraceMs has passed with no
//    success, fall back to the same active-scan-and-connect path as case 1
//    -- whatever a scan finds (the same keyboard re-advertising, or a
//    different one entirely) gets connected and, via
//    BleKeyboardHost::onLinkUp()->persistBonds(), becomes the new saved
//    bond going forward.
//
// Either way, a successful connect() persists the bond, so this function's
// scanning branch only ever actually runs while nothing usable is already
// connected or being retried.
static constexpr uint32_t kBleScanDurationMs = 5000;
static constexpr uint32_t kBleScanRetryDelayMs = 3000;
static constexpr uint32_t kBleBondedRetryGraceMs = 20000;

// File-scope (not function-local) so forceBlePairingNow() -- the BLE
// button's tap handler -- can reset them to skip straight to scanning,
// instead of waiting out whatever grace period bleKbdAutoPair() is
// currently in the middle of.
static uint32_t g_bleIdleSinceMs = 0;
static uint32_t g_bleNextScanAt = 0;
static bool g_bleScanArmed = false;

static void bleKbdAutoPair() {
  const uint32_t now = millis();

  if (BleHid.isConnected() || BleHid.isConnecting() || BleHid.isScanning()) {
    g_bleIdleSinceMs = 0;  // something's actively in flight; only measure genuine idle stretches
  } else if (g_bleIdleSinceMs == 0) {
    g_bleIdleSinceMs = now;
  }

  if (BleHid.isConnected() || BleHid.isConnecting()) return;
  if (BleHid.pairedCount() > 0 && (now - g_bleIdleSinceMs) < kBleBondedRetryGraceMs) return;

  if (BleHid.isScanning()) {
    g_bleScanArmed = true;
    return;
  }
  if (g_bleScanArmed) {
    g_bleScanArmed = false;
    for (uint8_t i = 0; i < BleHid.deviceCount(); i++) {
      const freeink::DiscoveredDevice& d = BleHid.device(i);
      if (d.hid && d.connectable) {
        BleHid.connect(d.addr);
        break;
      }
    }
    BleHid.releaseScanResults();
    g_bleNextScanAt = now + kBleScanRetryDelayMs;
    return;
  }
  if (now >= g_bleNextScanAt) {
    BleHid.startScan(kBleScanDurationMs);
    // Safety net in case isScanning() never reads true on some call (e.g.
    // begin() not actually running) -- without this a stalled scan would
    // permanently block retries rather than just wasting one cycle.
    g_bleNextScanAt = now + kBleScanDurationMs + kBleScanRetryDelayMs;
  }
}

// The BLE button's tap action (see BLE_TOGGLE_* / drawBleButton() /
// handleTouchTap()): drop whatever's currently connected and jump straight
// to scanning on the very next bleKbdAutoPair() call, rather than silently
// waiting out kBleBondedRetryGraceMs. Lets someone pair a *different*
// keyboard on demand without waiting for the current one to time out, and
// gives an immediate, visible response to the tap.
static void forceBlePairingNow() {
  // BLE starts at boot, so this normally never fires. Kept as the recovery
  // path: if the stack ever ends up down (a failed begin(), or a future
  // caller tearing it down), the BLE button is what brings it back rather
  // than a reboot.
  if (!BleHid.isRunning()) {
    BleHid.begin(MACHINE_NAME);
    g_bleIdleSinceMs = 0;
    g_bleNextScanAt = 0;
    g_bleScanArmed = false;
    return;
  }
  if (BleHid.isConnected()) BleHid.disconnect();
  g_bleIdleSinceMs = 1;  // nonzero and already older than kBleBondedRetryGraceMs
  g_bleNextScanAt = 0;
  g_bleScanArmed = false;
}

void setup() {
#if !FREEINK_CAP_USB_HID_KBD_HOST
  Serial.begin(115200);
  // Native USB CDC: the host needs time to enumerate before anything printed
  // here can reach it. A short delay loses the whole setup() trace.
  delay(3000);
#endif
  // With the USB HID Host capability on, Serial.begin() above is skipped on
  // purpose: it and UsbKbd.begin() (below) both want the same native-USB PHY
  // (see UsbHidKeyboardHost.h's own comment -- ESP32-S3 has exactly one, and
  // USB-Serial-JTAG/CDC and USB-OTG Host are mutually exclusive on it).
  // Calling Serial.print* without begin() is a normal, harmless no-op, so
  // every other STEP()/Serial.printf() call below stays as-is rather than
  // being conditionally compiled out one by one.
  Serial.println("\n=== " MACHINE_NAME " ===");
  Serial.flush();
#define STEP(msg) do { Serial.printf("[step] %s\n", msg); Serial.flush(); } while (0)

  // Match the init order of the reader that already drives this panel:
  // power rails held first, then GPIO, then the display.
  STEP("holdPowerRails");
  BoardConfig::holdPowerRails();
  STEP("gpio.begin");
  gpio.begin();
  // The reader brings power management up before it ever touches the panel.
  // Skipping it was the one init step this bring-up had that cpr does not.
  STEP("powerManager.begin");
  powerManager.begin();

  STEP("display.begin");
  display.begin();

STEP("renderer.begin");
  renderer.begin();
  // Landscape, native panel orientation -- see this file's header comment.
  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);

STEP("fonts");
  fontDecompressor.init();
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);

  renderer.insertFont(FONT_UI, uiFamily);
  renderer.insertFont(FONT_BODY, bodyFamily);
  renderer.insertFont(FONT_LARGE, titleFamily);
  renderer.insertFont(FONT_SMALL, smallFamily);
  renderer.insertFont(FONT_SCREEN_MONO_0, screenMono0Family);
  renderer.insertFont(FONT_SCREEN_MONO_1, screenMono1Family);
  renderer.insertFont(FONT_SCREEN_MONO_2, screenMono2Family);
  renderer.insertFont(FONT_SCREEN_MONO_3, screenMono3Family);

  // Needs FONT_UI registered above -- it measures the labels.
  layoutStatusBar();

STEP("input.begin");
  input.begin();
STEP("probeSdCard");
  probeSdCard();

  Serial.printf("logical %dx%d, touch=%d\n", renderer.getScreenWidth(), renderer.getScreenHeight(),
                (int)input.hasTouch());
  Serial.println(sdLine);

STEP("osk.init");
  oskInit(renderer, FONT_UI, FONT_SMALL, OSK_X, OSK_Y, OSK_W, OSK_H, onOskKey);

STEP("inputSetup");
  inputSetup();

STEP("UsbKbd.begin");
  // No-op (returns false) when FREEINK_CAP_USB_HID_KBD_HOST is off -- see
  // UsbHidKeyboardHost.h. Safe to call unconditionally either way.
  UsbKbd.begin();

STEP("BleHid.begin");
  // Started at boot again, so a keyboard that was paired before is connected
  // by the time anyone reaches for it. It was made tap-to-start while the
  // WiFi-connect failure was still being chased, to take BLE out of the
  // picture; the cause turned out to be a full NVS partition instead (see
  // docs/DEVELOPMENT_LOG.md), so the restriction had nothing to do with it.
  // loadBonds() inside begin() means poll() starts reconnecting to a saved
  // bond immediately; bleKbdAutoPair() below drives first-time pairing.
  BleHid.begin(MACHINE_NAME);

STEP("sdDateTime");
  // Before anything writes to the card, so no file is created dateless.
  sdDateTimeSetup();

STEP("editorInit");
  editorInit();
  editorSetGlyphWidthFn(editorGlyphWidth);

STEP("ota_apps");
  // Registering the name is what turns "OTA Slot 1" into "MicroBASIC" in the
  // reader's own switch menu -- it reads this out of shared NVS, so it takes
  // effect on ITS next boot, with nothing to reflash on that side.
  #if MICROWRITER
  registerOtaAppName("MicroWriter");
#else
  registerOtaAppName("MicroBASIC");
#endif
  {
    OtaAppEntry apps[MAX_OTA_APPS];
    const int n = detectOtaApps(apps, MAX_OTA_APPS);
    if (n > 0) {
      g_readerSubtype = apps[0].partitionSubtype;
      strncpy(g_readerName, apps[0].name, sizeof(g_readerName) - 1);
      g_readerName[sizeof(g_readerName) - 1] = '\0';
    }
    Serial.printf("[ota] sibling apps=%d target=0x%02X name=%s\n", n, g_readerSubtype, g_readerName);
  }

#if !MICROWRITER
STEP("screenEditorSetMode");
  screenEditorSetMode(2);  // SCREEN 2 (64-col), this panel's default -- see README
#endif

STEP("tbSetup");
#if MICROWRITER
  // The writing machine has no prompt to return to, so the browser is not a
  // screen you open, it is the screen. Nothing ever closes it.
  browserStart();
#else
  tbSetup();  // prints the boot banner into the terminal via screenEditorTermPrintLine
#endif

STEP("drawScreen");
  drawScreen();
  STEP("setup done");
}

static void printDiagnostics(const char* tag) {
  Serial.printf("[%s] logical %dx%d  orientation=%d  touch=%d  firstPaintDone=%d\n", tag,
                renderer.getScreenWidth(), renderer.getScreenHeight(),
                (int)renderer.getOrientation(), (int)input.hasTouch(), (int)firstPaintDone);
  Serial.printf("[%s] %s\n", tag, sdLine);
  Serial.printf("[%s] asyncRefresh=%d  heap=%u  psram=%u\n", tag,
                (int)display.supportsAsyncRefresh(), (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getFreePsram());
}

void loop() {
  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > 3000) {
    lastBeat = millis();
    printDiagnostics("beat");
  }

  bleKbdAutoPair();
  wifiSyncLoop();  // no-op unless the WiFi setup screen is active
  browserLoop();   // no-op unless the editor is open; drives auto-save

  // Runs any autoexec.bas basicSetup() found at startup. Deliberately here,
  // not in setup() -- see tb_bridge.h's own comment: a launcher-style
  // program runs forever, and running it inside setup() means loop() (which
  // reads touch, redraws, and keeps the interface alive) never starts.
  // Internally guarded (checks st == SRUN) so calling this every iteration
  // after the first is a cheap no-op.
#if !MICROWRITER
  tbRunPendingAutoexec();
#endif

  uint8_t kbdCode, kbdMods;
  bool kbdPressed;
  while (UsbKbd.popKey(kbdCode, kbdMods, kbdPressed)) {
    enqueueKeyEvent(kbdCode, kbdMods, kbdPressed);
  }

  static bool wasBleConnected = false;
  BleHid.poll();
  freeink::KeyEvent bleEv;
  while (BleHid.popKey(bleEv)) {
    enqueueKeyEvent(bleEv.keycode, bleEv.mods, bleEv.pressed);
  }
  // The BLE button reflects live connection state (see drawBleButton()),
  // but that state changes asynchronously -- a keyboard can connect or time
  // out with nobody touching the screen. Without this, the button would
  // only catch up to reality on whatever unrelated redraw happened next.
  if (BleHid.isConnected() != wasBleConnected) {
    wasBleConnected = BleHid.isConnected();
    screenDirty = true;
  }

  // Some peripherals (security-conscious ones especially -- e.g. business
  // keyboards) require Passkey Entry pairing instead of Just Works: the
  // *host* displays a 6-digit code and the user types it on the
  // *peripheral's own keys* to confirm. NimBLEDevice::setSecurityPasskey()
  // fixes ours at 123456 (see BleKeyboardHost::begin()), so the code is
  // always the same, but nothing surfaced it anywhere -- a keyboard
  // requiring this method would sit waiting for a code nobody was ever
  // shown, reading as "won't pair" with no visible error. Print it to the
  // terminal itself, same as the boot banner and every other message here.
  uint32_t passkey;
  if (BleHid.takePairingPasskey(passkey)) {
    char msg[48];
    snprintf(msg, sizeof(msg), "[ble] pairing code: %06lu (type on keyboard)",
              (unsigned long)passkey);
    notify(msg);
    screenDirty = true;
  }

  input.update();

  float nx, ny;
  if (input.wasTouchTap(nx, ny)) {
    int lx, ly;
    renderer.tapToLogical(nx, ny, lx, ly);
    Serial.printf("tap norm(%.3f, %.3f) -> logical(%d, %d)\n", nx, ny, lx, ly);
    if (handleTouchTap(lx, ly)) screenDirty = true;
  }

  // While the WiFi setup screen is up, every key goes to syncHandleKey()
  // instead of the screen editor -- same underlying queue, different
  // dispatch (see dequeueKeyEventForCaller()'s own comment).
  // This chain must stay in the same order as drawScreen()'s, or keys go to a
  // screen that is not the one being drawn. They disagreed once: the browser
  // was checked first here and second there, which nothing noticed until
  // MicroWriter, where the browser is always open -- SYNC drew but could not
  // be typed into or escaped from, because its keys were being handed to the
  // browser behind it.
  if (g_readerConfirm) {
    // Same modal rule as the tap handler: keys go to the confirmation and
    // nowhere else, so nothing types into the terminal behind it.
    uint8_t rCode, rMods;
    bool rPressed;
    while (dequeueKeyEventForCaller(rCode, rMods, rPressed)) {
      if (!rPressed) continue;
      if (rCode == HID_KEY_ENTER) {
        readerConfirmAccept();
        break;
      }
      if (rCode == HID_KEY_ESCAPE) {
        readerConfirmCancel();
        break;
      }
    }
  } else if (isWifiSyncActive()) {
    uint8_t wCode, wMods;
    bool wPressed;
    while (dequeueKeyEventForCaller(wCode, wMods, wPressed)) {
      if (wPressed) syncHandleKey(wCode, wMods);
    }
  } else if (isBrowserActive()) {
    uint8_t bCode, bMods;
    bool bPressed;
    while (dequeueKeyEventForCaller(bCode, bMods, bPressed)) {
      if (bPressed) browserHandleKey(bCode, bMods);
    }
  }
#if !MICROWRITER
  else {
    processAllInput();
  }
#endif

  if (screenDirty) {
    drawScreen();
    screenDirty = false;
  }
  delay(10);
}
