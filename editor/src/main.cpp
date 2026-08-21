// MicroBASIC-PaperS3 -- milestone 1/2: hardware bring-up + SCREEN font
// legibility test.
//
// Milestone 1 (display, GT911 touch, SD card, 4-corner tap mapping) is done
// and confirmed on real hardware -- see the project README and git log.
// FONT_SCREEN_MONO_0/1 are the two real SCREEN fonts this device will use
// (research/fonts/tools/emit_epdfont_header.py), and drawTerminalContent()
// fills the entire 48x24 terminal area with real glyphs -- not reference
// lines -- as the actual legibility test: if the 11x22 cell is too small to
// read on the physical panel, that has to be discovered HERE, since it's the
// one decision the rest of the port depends on.
//
// Layout: a 48x24 character terminal over a fixed on-screen keyboard area
// (unbuilt -- outline only, milestone 3):
//
//     540 x 960 portrait
//     +-----------------+
//     | 6|  48 x 24   |6|   terminal 528 x 528, cell 11 x 22
//     |  |  cell 11x22| |   (48*11 = 528, 24*22 = 528; 6px side margins)
//     +-----------------+
//     |    keyboard     |   540 x 432
//     +-----------------+
//
// All diagnostic/status text (panel size, touch/SD status, tap feedback)
// lives in the keyboard area on NotoSans/Ubuntu (FONT_UI/FONT_TITLE) instead
// of overlapping the terminal, so the terminal is an honest, uncluttered
// test of the mono fonts alone.
//
// Touch: tap the four corner marks -- feedback (a crosshair, plus the last
// tap's coordinates) draws in the keyboard area. Already confirmed correct
// on hardware; this is a standing regression check, not an open question.

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
static int tapCount = 0;
static int lastTapX = -1, lastTapY = -1;

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

// The real legibility test: every row of the 48x24 grid filled with actual
// SCREEN 1 (11x22) glyphs, not reference lines. Content is chosen to stress
// exactly what generate_screen_fonts.py's own docstring flags as fragile at
// a non-integer scale (1.375x here) -- stem width on I/l/T/#/*/}, and the
// Latin-1 accented set (BASIC text will be Portuguese). Cycles to fill all
// 24 rows; repetition is fine, this is a visual check, not a content demo.
static const char* const kTermLines[] = {
    "MicroBASIC PaperS3   SCREEN 1  48x24",
    "cell 11x22   the quick brown fox jumps",
    "over the lazy dog.  THE QUICK BROWN FOX",
    "JUMPS OVER THE LAZY DOG.",
    "0123456789   !@#$%^&*()_+-=[]{}",
    ":;\"'<>,.?/\\|~`",
    "stems: l I 1 | ! T # * } { [ ] ( )",
    "ç Ç ã Ã é É á Á â Â ê Ê í Í ó Ó ô Ô",
    "ú Ú ü Ü õ Õ ° ª º « » ¿ ¡",
};
static constexpr int kTermLineCount = sizeof(kTermLines) / sizeof(kTermLines[0]);

static void drawTerminalContent() {
  for (int row = 0; row < TERM_ROWS; row++) {
    renderer.drawText(FONT_SCREEN_MONO_1, TERM_X, TERM_Y + row * CELL_H,
                       kTermLines[row % kTermLineCount]);
  }
}

// Four corner marks for the GT911 flipX/flipY check.
static void drawCornerTargets() {
  const int m = 24;
  const int pts[4][2] = {{m, m}, {PANEL_W - m, m}, {m, PANEL_H - m}, {PANEL_W - m, PANEL_H - m}};
  for (auto& p : pts) {
    renderer.drawLine(p[0] - 10, p[1], p[0] + 10, p[1], true);
    renderer.drawLine(p[0], p[1] - 10, p[0], p[1] + 10, true);
  }
}

static void drawScreen() {
  renderer.clearScreen();

  // Terminal area: pure SCREEN 1 glyph content, nothing else drawn into it,
  // so it's an honest legibility test -- no UI chrome overlapping the cells.
  drawTerminalContent();
  drawCornerTargets();

  // Everything diagnostic lives in the keyboard area instead, on FONT_UI
  // (NotoSans/Ubuntu, kept specifically for anything that must stay readable
  // regardless of how the mono fonts turn out). At 540 logical px wide, the
  // reader's 14pt body font overflows on any line of real length -- that
  // produced 819 "Outside range" clips earlier -- so this stays on FONT_UI.
  char buf[96];
  int y = KBD_Y + 10;
  renderer.drawRect(0, KBD_Y, PANEL_W, KBD_H, true);

  renderer.drawText(FONT_TITLE, TERM_X, y, "MicroBASIC PaperS3");
  y += 26;

  snprintf(buf, sizeof(buf), "panel %dx%d  term %dx%d cell %dx%d", renderer.getScreenWidth(),
           renderer.getScreenHeight(), TERM_COLS, TERM_ROWS, CELL_W, CELL_H);
  renderer.drawText(FONT_UI, TERM_X, y, buf);
  y += 20;

  snprintf(buf, sizeof(buf), "touch: %s", input.hasTouch() ? "GT911 ok" : "NOT CONFIGURED");
  renderer.drawText(FONT_UI, TERM_X, y, buf);
  y += 20;

  renderer.drawText(FONT_UI, TERM_X, y, sdLine);
  y += 24;

  if (tapCount == 0) {
    renderer.drawText(FONT_UI, TERM_X, y, "Tap the 4 corner marks.");
  } else {
    snprintf(buf, sizeof(buf), "tap #%d -> (%d, %d)", tapCount, lastTapX, lastTapY);
    renderer.drawText(FONT_UI, TERM_X, y, buf);
  }

  // crosshair at the last tap
  if (lastTapX >= 0) {
    renderer.drawLine(lastTapX - 16, lastTapY, lastTapX + 16, lastTapY, 3, true);
    renderer.drawLine(lastTapX, lastTapY - 16, lastTapX, lastTapY + 16, 3, true);
  }

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
    renderer.tapToLogical(nx, ny, lastTapX, lastTapY);
    tapCount++;
    Serial.printf("tap #%d norm(%.3f, %.3f) -> logical(%d, %d)\n", tapCount, nx, ny, lastTapX,
                  lastTapY);
    drawScreen();
  }
  delay(10);
}
