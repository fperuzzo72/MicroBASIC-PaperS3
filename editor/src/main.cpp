// MicroBASIC-PaperS3 -- milestone 1: hardware bring-up.
//
// Proves the three subsystems the whole port stands on, on the real panel,
// before a single MicroBASIC feature is carried over: the 960x540 EPD driven
// in PORTRAIT (540x960 logical), the GT911 touch panel, and the SPI SD card.
//
// It doubles as the geometry proof. The decided layout is a 48x24 character
// terminal over a fixed on-screen keyboard:
//
//     540 x 960 portrait
//     +-----------------+
//     | 6|  48 x 24   |6|   terminal 528 x 528, cell 11 x 22
//     |  |  cell 11x22| |   (48*11 = 528, 24*22 = 528; 6px side margins)
//     +-----------------+
//     |    keyboard     |   540 x 432
//     +-----------------+
//
// so this draws the real cell grid at the real size. If 11x22 cells are too
// small to read on the physical panel, that has to be discovered HERE -- it
// is the one decision every font regeneration downstream depends on.
//
// Touch reports land in the terminal area as text and as a crosshair, which
// is also the corner-tap check freeink-sdk/docs/m5papers3-support.md asks for
// under "Still to verify": the GT911 flipX/flipY values in
// BoardConfig::M5PAPERS3_GT911 are inherited from M5Paper v1.1 by analogy and
// have never been confirmed on this chip revision. Tap the four corner marks;
// if the crosshair lands mirrored, those flags are wrong.

#include <Arduino.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <InputManager.h>
#include <SDCardManager.h>
#include <builtinFonts/notosans_12_regular.h>
#include <builtinFonts/notosans_14_regular.h>
#include <builtinFonts/notosans_16_bold.h>

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

// The cell grid, drawn at true size. Every 8th column and every 4th row gets a
// full rule so columns/rows can be counted straight off the panel.
static void drawCellGrid() {
  for (int c = 0; c <= TERM_COLS; c += 8) {
    renderer.drawLine(TERM_X + c * CELL_W, TERM_Y, TERM_X + c * CELL_W, TERM_Y + TERM_H, true);
  }
  for (int r = 0; r <= TERM_ROWS; r += 4) {
    renderer.drawLine(TERM_X, TERM_Y + r * CELL_H, TERM_X + TERM_W, TERM_Y + r * CELL_H, true);
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

  char buf[96];
  int y = 8;

  renderer.drawText(FONT_TITLE, TERM_X, y, "MicroBASIC PaperS3");
  y += 30;

  // Everything below stays on FONT_UI: at 540 logical pixels of width, the
  // reader's 14pt body font overflows the panel on any line of real length
  // (its glyphs are far wider than the X4-era mono cells this layout was
  // sketched against), which is what produced 819 "Outside range" clips.
  snprintf(buf, sizeof(buf), "panel %dx%d portrait", renderer.getScreenWidth(),
           renderer.getScreenHeight());
  renderer.drawText(FONT_UI, TERM_X, y, buf);
  y += 22;

  snprintf(buf, sizeof(buf), "term %dx%d  cell %dx%d = %dx%d", TERM_COLS, TERM_ROWS, CELL_W, CELL_H,
           TERM_W, TERM_H);
  renderer.drawText(FONT_UI, TERM_X, y, buf);
  y += 22;

  snprintf(buf, sizeof(buf), "touch: %s", input.hasTouch() ? "GT911 ok" : "NOT CONFIGURED");
  renderer.drawText(FONT_UI, TERM_X, y, buf);
  y += 22;

  renderer.drawText(FONT_UI, TERM_X, y, sdLine);
  y += 26;

  if (tapCount == 0) {
    renderer.drawText(FONT_UI, TERM_X, y, "Tap the 4 corner marks.");
  } else {
    snprintf(buf, sizeof(buf), "tap #%d -> (%d, %d)", tapCount, lastTapX, lastTapY);
    renderer.drawText(FONT_UI, TERM_X, y, buf);
  }

  drawCellGrid();
  drawCornerTargets();

  // keyboard area outline -- nothing lives here yet, this is milestone 2
  renderer.drawRect(0, KBD_Y, PANEL_W, KBD_H, true);
  snprintf(buf, sizeof(buf), "keyboard area %dx%d", PANEL_W, KBD_H);
  renderer.drawText(FONT_UI, TERM_X + 8, KBD_Y + KBD_H / 2 - 8, buf);

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
