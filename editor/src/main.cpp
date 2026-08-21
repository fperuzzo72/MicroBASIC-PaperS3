// MicroBASIC-PaperS3 -- milestones 1-3: hardware bring-up, SCREEN font
// legibility, on-screen keyboard.
//
// Milestones 1 (display, GT911 touch, SD card, 4-corner tap mapping) and 2
// (SCREEN 0/1 fonts, confirmed legible on the physical panel after fixing a
// row-packing bug in the font emitter) are done -- see the project README
// and git log.
//
// This build demonstrates the on-screen keyboard (osk.cpp/osk.h): the
// keyboard area renders real touch keys, and typing echoes into the
// terminal grid using SCREEN 1 (11x22). input_handler.cpp (the real
// enqueueKeyEvent() the ported editor will eventually use) isn't ported
// yet, so osk.cpp is deliberately standalone -- it calls a plain callback
// with (HID keycode, HID modifier byte), the exact wire format
// enqueueKeyEvent() already expects on the X4, so this whole component
// carries over unchanged once that file lands. See osk.h's top comment.
//
// Layout: a 48x24 character terminal over a fixed on-screen keyboard:
//
//     540 x 960 portrait
//     +-----------------+
//     | 6|  48 x 24   |6|   terminal 528 x 528, cell 11 x 22
//     |  |  cell 11x22| |   (48*11 = 528, 24*22 = 528; 6px side margins)
//     +-----------------+
//     |    keyboard     |   540 x 432
//     +-----------------+
//
// Touch corner-mapping and diagnostic status text (panel size, SD/touch
// status) were milestone 1's own on-screen checks -- both are confirmed and
// have been folded into Serial-only diagnostics (see printDiagnostics())
// rather than kept on the physical screen, which the keyboard now fully
// occupies.

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
#include <builtinFonts/unscii_11x22.h>
#include <builtinFonts/unscii_22x44.h>

#include "osk.h"

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

// --- decided layout (see the header comment) -------------------------------
static constexpr int PANEL_W = 540;  // logical, portrait
static constexpr int PANEL_H = 960;
static constexpr int TERM_COLS = 48;
static constexpr int TERM_ROWS = 24;
static constexpr int CELL_W = 11;
static constexpr int CELL_H = 22;
static constexpr int TERM_W = TERM_COLS * CELL_W;  // 528
static constexpr int TERM_H = TERM_ROWS * CELL_H;  // 528
static constexpr int TERM_X = (PANEL_W - TERM_W) / 2;  // 6
static constexpr int TERM_Y = 0;
static constexpr int KBD_Y = TERM_Y + TERM_H;      // 528
static constexpr int KBD_H = PANEL_H - KBD_Y;      // 432

static_assert(TERM_W == 528 && TERM_H == 528, "terminal geometry drifted");
static_assert(KBD_H == 432, "keyboard geometry drifted");

static constexpr int FONT_UI = -1559651934;     // notosans 12
static constexpr int FONT_BODY = -1014561631;   // notosans 14
static constexpr int FONT_TITLE = -1422711852;  // notosans 16
// Same numeric IDs the ported config.h used on the X4, kept for continuity
// when that file eventually comes across; only two SCREEN sizes exist here
// (see README's "SCREEN modes" table) so MONO_2/MONO_3 are not defined.
static constexpr int FONT_SCREEN_MONO_0 = -2000000001;  // SCREEN 0, 24 col, cell 22x44
static constexpr int FONT_SCREEN_MONO_1 = -2000000002;  // SCREEN 1, 48 col, cell 11x22 (default)

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
static SDCardManager sdCard;

static EpdFont uiRegularFont(&notosans_12_regular);
static EpdFont bodyRegularFont(&notosans_14_regular);
static EpdFont titleBoldFont(&notosans_16_bold);
static EpdFontFamily uiFamily(&uiRegularFont);
static EpdFontFamily bodyFamily(&bodyRegularFont);
static EpdFontFamily titleFamily(&titleBoldFont);

// MicroBASIC's own SCREEN fonts -- uncompressed, no FontDecompressor needed.
static EpdFont screenMono0Font(&unscii_22x44);
static EpdFont screenMono1Font(&unscii_11x22);
static EpdFontFamily screenMono0Family(&screenMono0Font);
static EpdFontFamily screenMono1Family(&screenMono1Font);

static char sdLine[96] = "SD: not probed";
static bool firstPaintDone = false;

static void probeSdCard() {
  if (!sdCard.begin()) {
    snprintf(sdLine, sizeof(sdLine), "SD: begin() FAILED");
    return;
  }
  int files = 0;
  FsFile dir = sdCard.open("/");
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

// --- typing echo buffer -----------------------------------------------
// Rows 0-1 of the terminal are a fixed banner; rows 2-23 (22 rows) are a
// live scroll buffer fed by the on-screen keyboard, proving the whole
// osk.cpp -> HID code -> character -> terminal loop end to end.
static constexpr int TYPE_ROW0 = 2;
static constexpr int TYPE_ROWS = TERM_ROWS - TYPE_ROW0;  // 22
static char g_typed[TYPE_ROWS][TERM_COLS + 1];
static int g_curRow = 0, g_curCol = 0;

static void typedGridReset() {
  for (int r = 0; r < TYPE_ROWS; r++) {
    memset(g_typed[r], ' ', TERM_COLS);
    g_typed[r][TERM_COLS] = '\0';
  }
  g_curRow = 0;
  g_curCol = 0;
}

static void typedGridScroll() {
  for (int r = 0; r < TYPE_ROWS - 1; r++) {
    memcpy(g_typed[r], g_typed[r + 1], TERM_COLS + 1);
  }
  memset(g_typed[TYPE_ROWS - 1], ' ', TERM_COLS);
  g_typed[TYPE_ROWS - 1][TERM_COLS] = '\0';
}

static void typedGridNewline() {
  g_curCol = 0;
  if (g_curRow + 1 < TYPE_ROWS) {
    g_curRow++;
  } else {
    typedGridScroll();
  }
}

static void typedGridBackspace() {
  if (g_curCol > 0) {
    g_curCol--;
    g_typed[g_curRow][g_curCol] = ' ';
  } else if (g_curRow > 0) {
    g_curRow--;
    g_curCol = TERM_COLS - 1;
    g_typed[g_curRow][g_curCol] = ' ';
  }
}

static void typedGridInsert(char c) {
  if (g_curCol >= TERM_COLS) typedGridNewline();
  g_typed[g_curRow][g_curCol] = c;
  g_curCol++;
}

// osk.cpp's callback: exactly the (HID keycode, HID modifiers) pair
// enqueueKeyEvent() will receive once input_handler.cpp is ported. Must be a
// plain function (OskKeyCallback is a raw pointer, not std::function) --
// no capture, all state is at module scope, matching how the real
// enqueueKeyEvent() is itself a free function over a module-scope queue.
static void onOskKey(uint8_t hidCode, uint8_t modifiers) {
  if (hidCode == OSK_HID_BACKSPACE) {
    typedGridBackspace();
    return;
  }
  if (hidCode == OSK_HID_LEFT) {
    if (g_curCol > 0) g_curCol--;
    return;
  }
  if (hidCode == OSK_HID_RIGHT) {
    if (g_curCol < TERM_COLS - 1) g_curCol++;
    return;
  }
  const char c = oskHidToChar(hidCode, modifiers);
  if (c == '\n') {
    typedGridNewline();
  } else if (c != 0) {
    typedGridInsert(c);
  }
}

static void drawTerminalContent() {
  renderer.drawText(FONT_SCREEN_MONO_1, TERM_X, TERM_Y, "MicroBASIC PaperS3  SCREEN 1  48x24");
  renderer.drawText(FONT_SCREEN_MONO_1, TERM_X, TERM_Y + CELL_H, "READY.");
  for (int r = 0; r < TYPE_ROWS; r++) {
    renderer.drawText(FONT_SCREEN_MONO_1, TERM_X, TERM_Y + (TYPE_ROW0 + r) * CELL_H, g_typed[r]);
  }
  // Solid-block cursor at the next insertion point -- always over blank
  // space (nothing to occlude), matching the emitter's own "cursor drawn
  // with fillRect() directly, not through drawText()" convention.
  renderer.fillRect(TERM_X + g_curCol * CELL_W, TERM_Y + (TYPE_ROW0 + g_curRow) * CELL_H, CELL_W,
                     CELL_H, true);
}

static void drawScreen() {
  renderer.clearScreen();
  drawTerminalContent();
  oskDraw();

  Serial.printf("[paint] displayBuffer(%s) ...\n", firstPaintDone ? "FAST" : "FULL");
  Serial.flush();
  const uint32_t t0 = millis();
  renderer.displayBuffer(firstPaintDone ? HalDisplay::FAST_REFRESH : HalDisplay::FULL_REFRESH);
  Serial.printf("[paint] returned after %lu ms\n", (unsigned long)(millis() - t0));
  Serial.flush();
  firstPaintDone = true;
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
  // Portrait is the decided orientation: the terminal sits above a fixed
  // keyboard, which only works on the long axis.
  renderer.setOrientation(GfxRenderer::Portrait);

STEP("fonts");
  fontDecompressor.init();
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);

  renderer.insertFont(FONT_UI, uiFamily);
  renderer.insertFont(FONT_BODY, bodyFamily);
  renderer.insertFont(FONT_TITLE, titleFamily);
  renderer.insertFont(FONT_SCREEN_MONO_0, screenMono0Family);
  renderer.insertFont(FONT_SCREEN_MONO_1, screenMono1Family);

STEP("input.begin");
  input.begin();
STEP("probeSdCard");
  probeSdCard();

  Serial.printf("logical %dx%d, touch=%d\n", renderer.getScreenWidth(), renderer.getScreenHeight(),
                (int)input.hasTouch());
  Serial.println(sdLine);

STEP("osk.init");
  typedGridReset();
  oskInit(renderer, FONT_UI, 0, KBD_Y, PANEL_W, KBD_H, onOskKey);

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

  input.update();

  float nx, ny;
  if (input.wasTouchTap(nx, ny)) {
    int lx, ly;
    renderer.tapToLogical(nx, ny, lx, ly);
    Serial.printf("tap norm(%.3f, %.3f) -> logical(%d, %d)\n", nx, ny, lx, ly);
    // oskHandleTap() returns true for any tap inside the keyboard area
    // (a modifier toggle or a normal key both need a redraw: the former to
    // show the key inverted, the latter to show the typed character and
    // moved cursor) and false outside it, where there is currently nothing
    // else to handle.
    if (oskHandleTap(lx, ly)) {
      drawScreen();
    }
  }
  delay(10);
}
