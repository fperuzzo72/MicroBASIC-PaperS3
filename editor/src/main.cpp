// MicroBASIC-PaperS3 -- milestones 1-3: hardware bring-up, SCREEN font
// legibility, on-screen keyboard.
//
// Milestone 1 (display, GT911 touch, SD card, 4-corner tap mapping) and
// milestone 2 (SCREEN fonts, confirmed legible after fixing a row-packing
// bug in the font emitter) are done -- see the project README and git log.
//
// Landscape, not portrait: the first portrait build (540 wide) worked but
// felt cramped on real hardware -- both the terminal glyphs and the
// keyboard keys read as too small. Landscape (960x540, matching the panel's
// native rotation=0 orientation -- GfxRenderer::LandscapeCounterClockwise is
// documented as exactly that, no further rotation on top) uses the full
// panel width for both. The terminal's 64x18 grid at cell 15x30 divides
// BOTH panel axes exactly (960/15=64, 540/30=18) with zero margin anywhere.
//
// The on-screen keyboard is no longer a permanently docked strip: with the
// terminal now claiming the whole panel, the keyboard is a toggleable
// OVERLAY covering the bottom 10 terminal rows when shown (see the "KBD"
// button, top-right). This matches the intended real policy -- a paired
// BLE keyboard is preferred, and the on-screen keyboard is a reserve you
// summon only when you need it -- though nothing here actually detects a
// BLE keyboard yet (ble_keyboard.cpp isn't ported); the toggle demonstrates
// the show/hide mechanism the real policy will drive later.
//
// osk.cpp itself is unchanged from the portrait build: it takes its region
// as plain parameters, so widening it to the full 960px and repositioning
// it as an overlay was a call-site change only, not a component change.

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
#include <builtinFonts/unscii_15x30.h>

#include "dead_keys.h"
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
static constexpr int PANEL_W = 960;  // logical, landscape (native panel orientation)
static constexpr int PANEL_H = 540;
static constexpr int TERM_COLS = 64;
static constexpr int TERM_ROWS = 18;
static constexpr int CELL_W = 15;
static constexpr int CELL_H = 30;
static constexpr int TERM_W = TERM_COLS * CELL_W;  // 960 -- exactly PANEL_W
static constexpr int TERM_H = TERM_ROWS * CELL_H;  // 540 -- exactly PANEL_H
static constexpr int TERM_X = 0;
static constexpr int TERM_Y = 0;

static_assert(TERM_W == PANEL_W && TERM_H == PANEL_H, "terminal should fill the panel exactly");

// On-screen keyboard overlay: covers the bottom of the screen when shown,
// full panel width. 300px gives osk.cpp's 5 rows a 60px row height, matching
// its 60px unit width (960/16 units) for near-square keys; leaves 240px
// (8 terminal rows) visible above it.
static constexpr int OSK_H = 300;
static constexpr int OSK_Y = PANEL_H - OSK_H;  // 240
static constexpr int OSK_X = 0;
static constexpr int OSK_W = PANEL_W;

// Toggle button: top-right, 6 terminal columns wide x 1 row tall.
static constexpr int TOGGLE_COLS = 6;
static constexpr int TOGGLE_W = TOGGLE_COLS * CELL_W;  // 90
static constexpr int TOGGLE_H = CELL_H;                // 30
static constexpr int TOGGLE_X = PANEL_W - TOGGLE_W;
static constexpr int TOGGLE_Y = 0;

static constexpr int FONT_UI = -1559651934;     // notosans 12
static constexpr int FONT_BODY = -1014561631;   // notosans 14
static constexpr int FONT_TITLE = -1422711852;  // notosans 16
static constexpr int FONT_SMALL = -1246724383;  // ubuntu 10 (X4's own UI_10_FONT_ID)
// Same numeric ID convention (and column counts) the ported config.h used
// on the X4: MONO_0=32col, MONO_1=48col, MONO_2=64col, MONO_3=80col. All
// four landscape font assets are generated (research/fonts/tools/
// emit_epdfont_header.py), but only MONO_2 is registered below -- the X4
// itself boots into SCREEN_MONO_1 (48col) because that's the readable
// default at the X4's panel size; this device has enough room that 64col
// is the better default, so it boots straight into MONO_2. The other
// three aren't wired into the renderer yet since nothing here can switch
// SCREEN modes at runtime (no BASIC interpreter in this bring-up).
static constexpr int FONT_SCREEN_MONO_2 = -2000000003;  // SCREEN 2, 64 col, cell 15x30, default here

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
static EpdFont smallRegularFont(&ubuntu_10_regular);
static EpdFontFamily uiFamily(&uiRegularFont);
static EpdFontFamily bodyFamily(&bodyRegularFont);
static EpdFontFamily titleFamily(&titleBoldFont);
static EpdFontFamily smallFamily(&smallRegularFont);

// MicroBASIC's own SCREEN font -- uncompressed, no FontDecompressor needed.
static EpdFont screenMono2Font(&unscii_15x30);
static EpdFontFamily screenMono2Family(&screenMono2Font);

static char sdLine[96] = "SD: not probed";
static bool firstPaintDone = false;
static bool g_oskVisible = false;

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
// Rows 0-1 of the terminal are a fixed banner; rows 2-17 (16 rows) are a
// live scroll buffer fed by the on-screen keyboard, proving the whole
// osk.cpp -> HID code -> character -> terminal loop end to end.
static constexpr int TYPE_ROW0 = 2;
static constexpr int TYPE_ROWS = TERM_ROWS - TYPE_ROW0;  // 16
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

// dead_keys.h's composed results are UTF-8 (1-3 bytes), but g_typed stores
// one byte per cell -- matching screen_editor.h's own documented convention
// ("characters come out as Latin-1 bytes, the same one-byte-per-character
// encoding BASIC strings use throughout"), not UTF-8. Every result this
// table produces is either plain ASCII (the literal-dead-key entries) or a
// 2-byte UTF-8 sequence for a Latin-1 Supplement codepoint (0xC2/0xC3 lead
// byte), so this decode is exact for everything dead_keys.h can return --
// it is NOT a general UTF-8 decoder.
static uint8_t latin1FromUtf8(const char* s) {
  const uint8_t b0 = (uint8_t)s[0];
  if (b0 < 0x80) return b0;
  const uint8_t b1 = (uint8_t)s[1];
  return (uint8_t)(((b0 & 0x1F) << 6) | (b1 & 0x3F));
}

// Feeds one typed character through dead_keys.h's US-International
// composition and inserts whatever comes out. Recurses at most once: a
// requeued character can itself arm a NEW dead key (e.g. typing ' then `
// flushes ' literally and requeues `, which then arms as its own pending
// dead key), but dead_keys.h's own state machine guarantees that second
// call can't produce a further requeue on top of it.
static void insertProcessedChar(char c) {
  const char* result = deadKeyProcess(c);
  if (result == nullptr) {
    typedGridInsert(c);
    return;
  }
  if (result[0] != '\0') {
    typedGridInsert((char)latin1FromUtf8(result));
    const char requeued = deadKeyTakeRequeue();
    if (requeued != 0) insertProcessedChar(requeued);
  }
  // result[0] == '\0': dead key stored, nothing to insert yet.
}

// osk.cpp's callback: exactly the (HID keycode, HID modifiers) pair
// enqueueKeyEvent() will receive once input_handler.cpp is ported. Must be a
// plain function (OskKeyCallback is a raw pointer, not std::function) --
// no capture, all state is at module scope, matching how the real
// enqueueKeyEvent() is itself a free function over a module-scope queue.
static void onOskKey(uint8_t hidCode, uint8_t modifiers) {
  if (hidCode == OSK_HID_ESCAPE) {
    // dead_keys.h's own doc comment names Escape as one of the cases that
    // should cancel a pending dead key.
    deadKeyReset();
    return;
  }
  if (hidCode == OSK_HID_BACKSPACE) {
    deadKeyReset();
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
    deadKeyReset();
    typedGridNewline();
  } else if (c != 0) {
    insertProcessedChar(c);
  }
}

// g_typed stores Latin-1 (see insertProcessedChar()'s comment); drawText()
// wants UTF-8. Every stored byte is either plain ASCII or a Latin-1
// Supplement codepoint (0x80-0xFF), so each needing encoding is always
// exactly a 2-byte UTF-8 sequence -- worst case every column is accented,
// hence the 2x+1 buffer.
static void latin1RowToUtf8(const char* row, char* out, size_t outSize) {
  size_t o = 0;
  for (size_t i = 0; row[i] != '\0' && o + 3 < outSize; i++) {
    const uint8_t b = (uint8_t)row[i];
    if (b < 0x80) {
      out[o++] = (char)b;
    } else {
      out[o++] = (char)(0xC0 | (b >> 6));
      out[o++] = (char)(0x80 | (b & 0x3F));
    }
  }
  out[o] = '\0';
}

static void drawTerminalContent() {
  char banner[TERM_COLS + 1];
  snprintf(banner, sizeof(banner), "FSP MicroBASIC PaperS3   SCREEN 1  %dx%d", TERM_COLS,
           TERM_ROWS);
  renderer.drawText(FONT_SCREEN_MONO_2, TERM_X, TERM_Y, banner);
  renderer.drawText(FONT_SCREEN_MONO_2, TERM_X, TERM_Y + CELL_H, "READY.");
  char utf8Row[TERM_COLS * 2 + 1];
  for (int r = 0; r < TYPE_ROWS; r++) {
    const int rowY = TERM_Y + (TYPE_ROW0 + r) * CELL_H;
    // Rows the keyboard overlay covers are now drawn too, not skipped --
    // the overlay's key backgrounds are a sparse dot pattern that leaves
    // most of this text showing through (see osk.cpp's oskDraw()), so
    // there is something worth drawing under it. Costs a bit of extra draw
    // time on every refresh while the keyboard is up; not worth optimizing
    // away for a bring-up program.
    latin1RowToUtf8(g_typed[r], utf8Row, sizeof(utf8Row));
    renderer.drawText(FONT_SCREEN_MONO_2, TERM_X, rowY, utf8Row);
  }
  // Solid-block cursor at the next insertion point.
  const int cursorY = TERM_Y + (TYPE_ROW0 + g_curRow) * CELL_H;
  renderer.fillRect(TERM_X + g_curCol * CELL_W, cursorY, CELL_W, CELL_H, true);
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
  renderer.insertFont(FONT_TITLE, titleFamily);
  renderer.insertFont(FONT_SMALL, smallFamily);
  renderer.insertFont(FONT_SCREEN_MONO_2, screenMono2Family);

STEP("input.begin");
  input.begin();
STEP("probeSdCard");
  probeSdCard();

  Serial.printf("logical %dx%d, touch=%d\n", renderer.getScreenWidth(), renderer.getScreenHeight(),
                (int)input.hasTouch());
  Serial.println(sdLine);

STEP("osk.init");
  typedGridReset();
  oskInit(renderer, FONT_UI, FONT_SMALL, OSK_X, OSK_Y, OSK_W, OSK_H, onOskKey);

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

    if (tapInRect(lx, ly, TOGGLE_X, TOGGLE_Y, TOGGLE_W, TOGGLE_H)) {
      g_oskVisible = !g_oskVisible;
      drawScreen();
    } else if (g_oskVisible && oskHandleTap(lx, ly)) {
      // oskHandleTap() bounds-checks against the region passed to oskInit()
      // and returns false outside it; gated on g_oskVisible too so a tap
      // over the (currently hidden) overlay's fixed region doesn't get
      // misread as a key press while real terminal content is shown there.
      drawScreen();
    }
  }
  delay(10);
}
