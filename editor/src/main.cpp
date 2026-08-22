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

// Toggle button: top-right, fixed pixel size (not tied to the terminal's
// current cell size, unlike an earlier draft).
static constexpr int TOGGLE_W = 90;
static constexpr int TOGGLE_H = 30;
static constexpr int TOGGLE_X = PANEL_W - TOGGLE_W;
static constexpr int TOGGLE_Y = 0;

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

static bool tapInRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
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
  drawTerminalContent();
  drawToggleButton();
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
  input.update();
  float nx, ny;
  if (!input.wasTouchTap(nx, ny)) return;
  int lx, ly;
  renderer.tapToLogical(nx, ny, lx, ly);
  if (handleTouchTap(lx, ly)) screenEditorFlushDisplay();
}

void setup() {
  Serial.begin(115200);
  // Native USB CDC: the host needs time to enumerate before anything printed
  // here can reach it. A short delay loses the whole setup() trace.
  delay(3000);
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

  // Runs any autoexec.bas basicSetup() found at startup. Deliberately here,
  // not in setup() -- see tb_bridge.h's own comment: a launcher-style
  // program runs forever, and running it inside setup() means loop() (which
  // reads touch, redraws, and keeps the interface alive) never starts.
  // Internally guarded (checks st == SRUN) so calling this every iteration
  // after the first is a cheap no-op.
  tbRunPendingAutoexec();

  input.update();

  float nx, ny;
  if (input.wasTouchTap(nx, ny)) {
    int lx, ly;
    renderer.tapToLogical(nx, ny, lx, ly);
    Serial.printf("tap norm(%.3f, %.3f) -> logical(%d, %d)\n", nx, ny, lx, ly);
    if (handleTouchTap(lx, ly)) screenDirty = true;
  }

  processAllInput();

  if (screenDirty) {
    drawScreen();
    screenDirty = false;
  }
  delay(10);
}
