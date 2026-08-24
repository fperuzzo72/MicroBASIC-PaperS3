// MicroBASIC-PaperS3 -- milestones 1-4: hardware bring-up, SCREEN fonts,
// on-screen keyboard, screen editor + BASIC interpreter.
//
// Milestones 1-3 are done and confirmed on real hardware -- see the project
// README and git log. This build ports the real screen editor
// (screen_editor.cpp), the input/dead-key/line-editing logic
// (input_handler.cpp) and the BASIC interpreter bridge (tb_bridge.cpp,
// tb_runtime.cpp, editor/lib/TinyBasic) from MicroBASIC's own
// port-staging/, replacing the typing-echo demo the on-screen keyboard was
// previously wired to. The OSK's callback now feeds the real
// enqueueKeyEvent() instead of a standalone demo buffer -- exactly the
// swap osk.h's own top comment anticipated from the start.
//
// Not ported yet, and deliberately out of scope for this pass: the main
// menu, file browser, prose text editor, BLE keyboard host, WiFi sync. This
// device currently boots straight into the screen editor with no menu to
// return to -- see input_handler.cpp's TODO comments for exactly what's
// stubbed and why.

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
#include "screen_editor.h"
#include "tb_bridge.h"
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
// out right-to-left, most-used closest to the corner: BLE and KBD are live;
// SYNC, EDITOR and MENU are reserved slots -- drawn, tap-recognized, but not
// wired to anything yet, since those systems aren't ported (see the
// README's "Sibling project" section). STATUS_TITLE_W is computed from the
// leftmost reserved button's X, so it shrinks on its own as more buttons
// get added later rather than needing to be recalculated by hand.
static constexpr int STATUS_BAR_H = 30;
static constexpr int STATUS_BAR_Y = 0;
static constexpr int STATUS_BTN_W = 90;

static constexpr int BLE_TOGGLE_W = STATUS_BTN_W;
static constexpr int BLE_TOGGLE_H = STATUS_BAR_H;
static constexpr int BLE_TOGGLE_X = PANEL_W - BLE_TOGGLE_W;
static constexpr int BLE_TOGGLE_Y = STATUS_BAR_Y;

static constexpr int TOGGLE_W = STATUS_BTN_W;
static constexpr int TOGGLE_H = STATUS_BAR_H;
static constexpr int TOGGLE_X = BLE_TOGGLE_X - TOGGLE_W;
static constexpr int TOGGLE_Y = STATUS_BAR_Y;

static constexpr int SYNC_BTN_W = STATUS_BTN_W;
static constexpr int SYNC_BTN_H = STATUS_BAR_H;
static constexpr int SYNC_BTN_X = TOGGLE_X - SYNC_BTN_W;
static constexpr int SYNC_BTN_Y = STATUS_BAR_Y;

static constexpr int EDITOR_BTN_W = STATUS_BTN_W;
static constexpr int EDITOR_BTN_H = STATUS_BAR_H;
static constexpr int EDITOR_BTN_X = SYNC_BTN_X - EDITOR_BTN_W;
static constexpr int EDITOR_BTN_Y = STATUS_BAR_Y;

static constexpr int MENU_BTN_W = STATUS_BTN_W;
static constexpr int MENU_BTN_H = STATUS_BAR_H;
static constexpr int MENU_BTN_X = EDITOR_BTN_X - MENU_BTN_W;
static constexpr int MENU_BTN_Y = STATUS_BAR_Y;

// Everything left of the leftmost reserved button: outlined, holds the
// boot title until some future button claims part of it.
static constexpr int STATUS_TITLE_X = 0;
static constexpr int STATUS_TITLE_Y = STATUS_BAR_Y;
static constexpr int STATUS_TITLE_W = MENU_BTN_X - STATUS_TITLE_X;
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

static void drawToggleButton() {
  const int inset = 2;
  renderer.drawRect(TOGGLE_X + inset, TOGGLE_Y + inset, TOGGLE_W - 2 * inset, TOGGLE_H - 2 * inset,
                     true);
  const char* label = g_oskVisible ? "Hide" : "KBD";
  const int tw = renderer.getTextWidth(FONT_UI, label);
  const int tx = TOGGLE_X + (TOGGLE_W - tw) / 2;
  const int ty = TOGGLE_Y + (TOGGLE_H - renderer.getLineHeight(FONT_UI)) / 2;
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
    renderer.fillRect(BLE_TOGGLE_X + inset, BLE_TOGGLE_Y + inset, BLE_TOGGLE_W - 2 * inset,
                       BLE_TOGGLE_H - 2 * inset, true);
  } else {
    renderer.drawRect(BLE_TOGGLE_X + inset, BLE_TOGGLE_Y + inset, BLE_TOGGLE_W - 2 * inset,
                       BLE_TOGGLE_H - 2 * inset, true);
  }
  const char* label = "BLE";
  const int tw = renderer.getTextWidth(FONT_UI, label);
  const int tx = BLE_TOGGLE_X + (BLE_TOGGLE_W - tw) / 2;
  const int ty = BLE_TOGGLE_Y + (BLE_TOGGLE_H - renderer.getLineHeight(FONT_UI)) / 2;
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
    renderer.fillRect(SYNC_BTN_X + inset, SYNC_BTN_Y + inset, SYNC_BTN_W - 2 * inset,
                       SYNC_BTN_H - 2 * inset, true);
  } else {
    renderer.drawRect(SYNC_BTN_X + inset, SYNC_BTN_Y + inset, SYNC_BTN_W - 2 * inset,
                       SYNC_BTN_H - 2 * inset, true);
  }
  const char* label = "SYNC";
  const int tw = renderer.getTextWidth(FONT_UI, label);
  const int tx = SYNC_BTN_X + (SYNC_BTN_W - tw) / 2;
  const int ty = SYNC_BTN_Y + (SYNC_BTN_H - renderer.getLineHeight(FONT_UI)) / 2;
  renderer.drawText(FONT_UI, tx, ty, label, !active);
}

// Everything in the status bar: the reserved placeholder buttons, the
// live ones, and the outlined title area filling whatever's left of the
// bar (STATUS_TITLE_W already accounts for how many buttons exist).
static void drawStatusBar() {
  drawPlaceholderButton(MENU_BTN_X, MENU_BTN_Y, MENU_BTN_W, MENU_BTN_H, "MENU");
  drawPlaceholderButton(EDITOR_BTN_X, EDITOR_BTN_Y, EDITOR_BTN_W, EDITOR_BTN_H, "EDITOR");
  drawSyncButton();
  drawToggleButton();
  drawBleButton();

  const int inset = 2;
  renderer.drawRect(STATUS_TITLE_X + inset, STATUS_TITLE_Y + inset, STATUS_TITLE_W - 2 * inset,
                     STATUS_TITLE_H - 2 * inset, true);
  const char* title = "FSP MicroBASIC Paper S3 v0.3";
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
static constexpr int WIFI_UI_X = 0;
static constexpr int WIFI_UI_Y = STATUS_BAR_Y + STATUS_BAR_H;
static constexpr int WIFI_UI_W = PANEL_W;
static constexpr int WIFI_UI_H = OSK_Y - WIFI_UI_Y;

static void drawWifiCentered(const char* line1, const char* line2 = nullptr) {
  const int lh = renderer.getLineHeight(FONT_UI);
  const int ty1 = WIFI_UI_Y + WIFI_UI_H / 2 - (line2 ? lh : lh / 2);
  const int tw1 = renderer.getTextWidth(FONT_UI, line1);
  renderer.drawText(FONT_UI, WIFI_UI_X + (WIFI_UI_W - tw1) / 2, ty1, line1);
  if (line2) {
    const int tw2 = renderer.getTextWidth(FONT_UI, line2);
    renderer.drawText(FONT_UI, WIFI_UI_X + (WIFI_UI_W - tw2) / 2, ty1 + lh, line2);
  }
}

static void drawNetworkList() {
  const int rowH = renderer.getLineHeight(FONT_SMALL) + 4;
  const int visibleRows = WIFI_UI_H / rowH;
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

static bool tapInRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

// Declared in input_handler.h -- see its doc comment.
void startWifiSyncFromCommand() {
  if (isWifiSyncActive()) return;
  // The wizard's network list uses the top band (see drawWifiUi()) and its
  // password entry needs a keyboard -- shown for the whole session, not just
  // while typing, so there's one consistent layout throughout rather than
  // the screen jumping when password entry starts.
  g_oskVisible = true;
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
  if (tapInRect(lx, ly, TOGGLE_X, TOGGLE_Y, TOGGLE_W, TOGGLE_H)) {
    g_oskVisible = !g_oskVisible;
    return true;
  }
  if (tapInRect(lx, ly, BLE_TOGGLE_X, BLE_TOGGLE_Y, BLE_TOGGLE_W, BLE_TOGGLE_H)) {
    forceBlePairingNow();
    return true;
  }
  if (tapInRect(lx, ly, SYNC_BTN_X, SYNC_BTN_Y, SYNC_BTN_W, SYNC_BTN_H)) {
    startWifiSyncFromCommand();
    return true;
  }
  if (tapInRect(lx, ly, MENU_BTN_X, MENU_BTN_Y, MENU_BTN_W, MENU_BTN_H) ||
      tapInRect(lx, ly, EDITOR_BTN_X, EDITOR_BTN_Y, EDITOR_BTN_W, EDITOR_BTN_H)) {
    return false;  // reserved slot, not wired to anything yet
  }
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
  } else {
    drawTerminalContent();
  }
  drawStatusBar();
  if (g_oskVisible) oskDraw();

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
  // First tap ever (or first since the WiFi flow suspended it): BLE isn't
  // running yet, so there's nothing to disconnect or rescan -- just bring
  // the stack up. loadBonds() inside begin() means a previously-paired
  // keyboard still reconnects automatically once bleKbdAutoPair() (now
  // running, since isRunning() is true from here on) sees it.
  if (!BleHid.isRunning()) {
    BleHid.begin("MicroBASIC-PaperS3");
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
  Serial.println("\n=== MicroBASIC-PaperS3 bring-up ===");
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
  // Deliberately NOT started here. BLE now stays off until the BLE button is
  // tapped (forceBlePairingNow() calls BleHid.begin() then) -- starting it
  // unconditionally at boot meant it was also live (and scanning/reconnecting
  // to a bonded keyboard) during every WiFi attempt, which is exactly the
  // variable the WiFi-connect-failure investigation needs to rule out. See
  // docs/DEVELOPMENT_LOG.md.

STEP("screenEditorSetMode");
  screenEditorSetMode(2);  // SCREEN 2 (64-col), this panel's default -- see README

STEP("tbSetup");
  tbSetup();  // prints the boot banner into the terminal via screenEditorTermPrintLine

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

  // Only drives scanning/reconnecting once BLE has actually been started
  // (see setup()'s BleHid.begin() comment) -- a no-op the whole time nobody
  // has tapped the BLE button yet.
  if (BleHid.isRunning()) bleKbdAutoPair();
  wifiSyncLoop();  // no-op unless the WiFi setup screen is active

  // Runs any autoexec.bas basicSetup() found at startup. Deliberately here,
  // not in setup() -- see tb_bridge.h's own comment: a launcher-style
  // program runs forever, and running it inside setup() means loop() (which
  // reads touch, redraws, and keeps the interface alive) never starts.
  // Internally guarded (checks st == SRUN) so calling this every iteration
  // after the first is a cheap no-op.
  tbRunPendingAutoexec();

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
    screenEditorTermPrintLine(msg);
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
  if (isWifiSyncActive()) {
    uint8_t wCode, wMods;
    bool wPressed;
    while (dequeueKeyEventForCaller(wCode, wMods, wPressed)) {
      if (wPressed) syncHandleKey(wCode, wMods);
    }
  } else {
    processAllInput();
  }

  if (screenDirty) {
    drawScreen();
    screenDirty = false;
  }
  delay(10);
}
