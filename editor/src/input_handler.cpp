#include "input_handler.h"

#include <Arduino.h>

#include "dead_keys.h"
#include "screen_editor.h"
#include "tb_bridge.h"

#include <Utf8.h>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>

// Set by handleScreenEditorKey()/executeLogicalLine() below; read by main.cpp
// to decide whether a redraw is due. Also written by tb_bridge.cpp/
// tb_runtime.cpp whenever the interpreter itself changes what's on screen
// (PRINT output, LIST, errors) -- declared extern there too.
bool screenDirty = true;

// --- Input Queue -------------------------------------------------------
static KeyEvent inputQueue[INPUT_QUEUE_SIZE];
static int queueHead = 0;
static int queueTail = 0;
static volatile bool queueFull = false;

static bool capsLockOn = false;  // reserved: nothing currently sets this true

void inputSetup() {
  queueHead = 0;
  queueTail = 0;
  queueFull = false;
  capsLockOn = false;
}

static bool isQueueEmpty() { return (queueHead == queueTail) && !queueFull; }

void enqueueKeyEvent(uint8_t keyCode, uint8_t modifiers, bool pressed) {
  noInterrupts();
  if (!queueFull) {
    inputQueue[queueHead].keyCode = keyCode;
    inputQueue[queueHead].modifiers = modifiers;
    inputQueue[queueHead].pressed = pressed;
    queueHead = (queueHead + 1) % INPUT_QUEUE_SIZE;
    if (queueHead == queueTail) queueFull = true;
  }
  interrupts();
}

static KeyEvent dequeueKeyEvent() {
  KeyEvent event = {0, 0, false};
  noInterrupts();
  if (!isQueueEmpty()) {
    event = inputQueue[queueTail];
    queueTail = (queueTail + 1) % INPUT_QUEUE_SIZE;
    queueFull = false;
  }
  interrupts();
  return event;
}

char hidToAscii(uint8_t hid, uint8_t modifiers) {
  bool shifted = isShift(modifiers) ^ capsLockOn;

  if (hid >= 0x04 && hid <= 0x1D) {
    char base = 'a' + (hid - 0x04);
    return shifted ? (base - 32) : base;
  }

  static const char unshifted[] = "1234567890";
  static const char shiftedNum[] = "!@#$%^&*()";
  if (hid >= 0x1E && hid <= 0x27) {
    int idx = hid - 0x1E;
    return isShift(modifiers) ? shiftedNum[idx] : unshifted[idx];
  }

  switch (hid) {
    case 0x28: return '\n';  // Enter
    case 0x2B: return '\t';  // Tab
    case 0x2C: return ' ';   // Space

    case 0x2D: return isShift(modifiers) ? '_' : '-';
    case 0x2E: return isShift(modifiers) ? '+' : '=';
    case 0x2F: return isShift(modifiers) ? '{' : '[';
    case 0x30: return isShift(modifiers) ? '}' : ']';
    case 0x31: return isShift(modifiers) ? '|' : '\\';
    case 0x33: return isShift(modifiers) ? ':' : ';';
    case 0x34: return isShift(modifiers) ? '"' : '\'';
    case 0x35: return isShift(modifiers) ? '~' : '`';
    case 0x36: return isShift(modifiers) ? '<' : ',';
    case 0x37: return isShift(modifiers) ? '>' : '.';
    case 0x38: return isShift(modifiers) ? '?' : '/';

    default: return 0;
  }
}

// --- Screen editor: the only "screen" this build has --------------------
//
// executeLogicalLine()/handleScreenEditorKey() are ported from
// input_handler.cpp almost unchanged; MENU/EXIT and VC are commented out
// rather than wired to a menu/vc_browser.h that doesn't exist yet in this
// port. Typing those words does whatever the interpreter itself does with
// an unrecognized statement (a syntax error) until they're ported.

static void executeLogicalLine(const char* line) {
  const char* p = line;
  while (*p == ' ') p++;

  if (!*p) {
    screenEditorStartNewInputLine();
    return;
  }

  char upper[MAX_PROGRAM_LINE_LEN];
  int n = 0;
  for (; p[n] && n < (int)sizeof(upper) - 1; n++) upper[n] = (char)toupper((unsigned char)p[n]);
  upper[n] = '\0';

  auto isWord = [&](const char* w) { return strcmp(upper, w) == 0; };
  auto wordArg = [&](const char* w) -> const char* {
    size_t wl = strlen(w);
    if (strncmp(upper, w, wl) != 0) return nullptr;
    if (p[wl] != ' ' && p[wl] != '\0') return nullptr;
    const char* arg = p + wl;
    while (*arg == ' ') arg++;
    return arg;  // may point at '\0' if no argument was given
  };

  // TODO(menu): MENU/EXIT return to the main menu on the X4 -- no menu
  // exists yet in this port.
  // TODO(vc_browser): VC opens the full-screen program picker on the X4 --
  // vc_browser.h isn't ported yet.

  if (isWord("SYNC")) {
    // Same entry point as tapping the status bar's SYNC button -- lets
    // someone start the WiFi transfer flow without touching the screen.
    screenEditorStartNewInputLine();
    startWifiSyncFromCommand();
    return;
  }
  if (isWord("FILES") || isWord("DIR")) {
    // Aliases: the interpreter calls its directory listing CATALOG. FILES is
    // this project's own name, DIR the one anyone from CP/M or DOS reaches
    // for first.
    screenEditorStartNewInputLine();
    tbExecuteLine("CATALOG");
    if (screenEditorGetCursorCol() != 0) screenEditorStartNewInputLine();
    return;
  }
  if (const char* arg = wordArg("SCREEN")) {
    // Ours, not the language's: the interpreter has no SCREEN, and these
    // are this project's own 32/48/64/80-column text modes (see README).
    screenEditorStartNewInputLine();
    if (!*arg) {
      char msg[32];
      snprintf(msg, sizeof(msg), "SCREEN %d", screenEditorGetMode());
      screenEditorTermPrintLine(msg);
    } else {
      const int mode = atoi(arg);
      if (mode < 0 || mode > 3) {
        screenEditorTermPrintLine("?Bad SCREEN mode");
      } else {
        screenEditorSetMode(mode);
      }
    }
    if (screenEditorGetCursorCol() != 0) screenEditorStartNewInputLine();
    return;
  }

  // Everything else -- including numbered lines, LIST, RUN, SAVE, LOAD, NEW --
  // is the interpreter's. Its own output (listings, errors, PRINT) reaches
  // the terminal through the runtime's outch().
  screenEditorStartNewInputLine();
  tbExecuteLine(p);
  if (screenEditorGetCursorCol() != 0) screenEditorStartNewInputLine();
}

static void handleScreenEditorKey(uint8_t keyCode, uint8_t modifiers) {
  // TODO(menu): Escape returns to the main menu on the X4 -- no menu exists
  // yet in this port, so it's a no-op here (falls through: not a navigation
  // key below, and hidToAscii() returns 0 for it, so nothing is inserted
  // either). A running program's own Escape-to-break still works --  that
  // path is pumpProgramInput()'s, not this one.

  switch (keyCode) {
    case HID_KEY_LEFT:      screenEditorMoveCursor(0, -1);  screenDirty = true; return;
    case HID_KEY_RIGHT:     screenEditorMoveCursor(0, 1);   screenDirty = true; return;
    case HID_KEY_UP:        screenEditorMoveCursor(-1, 0);  screenDirty = true; return;
    case HID_KEY_DOWN:      screenEditorMoveCursor(1, 0);   screenDirty = true; return;
    case HID_KEY_HOME:      screenEditorGoHome();           screenDirty = true; return;
    case HID_KEY_END:       screenEditorGoEnd();            screenDirty = true; return;
    case HID_KEY_PAGE_UP:   screenEditorGoFirstRow();       screenDirty = true; return;
    case HID_KEY_PAGE_DOWN: screenEditorGoLastRow();        screenDirty = true; return;
    case HID_KEY_BACKSPACE: screenEditorBackspace();        screenDirty = true; return;
    case HID_KEY_ENTER: {
      char line[MAX_PROGRAM_LINE_LEN];
      screenEditorGetLogicalLineText(line, sizeof(line));
      executeLogicalLine(line);
      screenDirty = true;
      return;
    }
  }

  // Printable character -- the same US-International dead key engine as the
  // (not yet ported) prose editor, so accented input works here too.
  char c = hidToAscii(keyCode, modifiers);
  if (c != 0) {
    const char* composed = deadKeyProcess(c);
    if (composed == nullptr) {
      screenEditorInsertCodepoint((uint32_t)(unsigned char)c);
      screenDirty = true;
    } else if (composed[0] != '\0') {
      const unsigned char* p = (const unsigned char*)composed;
      uint32_t cp = utf8NextCodepoint(&p);
      if (cp != 0) screenEditorInsertCodepoint(cp);
      screenDirty = true;
      char req = deadKeyTakeRequeue();
      if (req != 0) screenEditorInsertCodepoint((uint32_t)(unsigned char)req);
    }
    // else: composed[0] == '\0' -> dead key stored, nothing to insert yet
  }
}

// --- Program-key ring buffer + break detection --------------------------
//
// Separate from the main dispatch queue above: BASIC's GET/@A/@C and the
// interpreter's checkch()/consins() read from THIS ring, one byte at a
// time, while handleScreenEditorKey() above handles line editing at the
// prompt. Only one of the two is ever draining the main hardware queue at
// once -- see pumpProgramInput()'s own comment -- so there is no race
// between them despite sharing the same source queue.

static char progKeys[16];
static uint8_t progHead = 0;
static uint8_t progTail = 0;
static bool breakPending = false;

static constexpr uint8_t PROG_KEYS_SIZE = (uint8_t)sizeof(progKeys);

// One character into the ring. Characters are *bytes*: an accented one is a
// single Latin-1 byte, not the two UTF-8 bytes the editors use, because
// BASIC strings are byte arrays -- LEN() and MID$ have to agree with what
// was actually typed. tb_runtime.cpp's outch() converts back to UTF-8 on
// the way to the screen.
static void pushProgramKey(char c) {
  const uint8_t next = (uint8_t)((progHead + 1) % PROG_KEYS_SIZE);
  if (next == progTail) return;  // full: drop the newest
  progKeys[progHead] = c;
  progHead = next;
}

// Both inputConsumeBreakPending() and the GET path call this, because the
// interpreter reaches them at different moments and whichever runs first
// has to be the one that empties the hardware queue.
static void pumpProgramInput() {
  while (!isQueueEmpty()) {
    KeyEvent event = dequeueKeyEvent();
    if (!event.pressed) continue;

    if (event.keyCode == HID_KEY_ESCAPE ||
        (event.keyCode == HID_KEY_C && isCtrl(event.modifiers))) {
      breakPending = true;
      continue;  // a break is not also a character
    }

    char c = hidToAscii(event.keyCode, event.modifiers);
    if (c == 0) {
      // Arrow keys have no ASCII, and a program that reads the keyboard at
      // all almost certainly wants them -- the MSX codes, matching this
      // project's SCREEN modes and editing model.
      switch (event.keyCode) {
        case HID_KEY_RIGHT: c = 28; break;
        case HID_KEY_LEFT:  c = 29; break;
        case HID_KEY_UP:    c = 30; break;
        case HID_KEY_DOWN:  c = 31; break;
        case HID_KEY_BACKSPACE: c = 8; break;  // ASCII backspace, for INPUT
        default: continue;
      }
      pushProgramKey(c);
      continue;
    }

    // Same US-International dead key engine the screen editor uses, so
    // INPUT takes accents too.
    const char* composed = deadKeyProcess(c);
    if (composed == nullptr) {
      pushProgramKey(c);
      continue;
    }
    if (composed[0] == '\0') continue;  // dead key held, nothing typed yet

    const unsigned char* u = (const unsigned char*)composed;
    const uint32_t cp = utf8NextCodepoint(&u);
    if (cp == 0) continue;
    if (cp <= 0xFF) pushProgramKey((char)(unsigned char)cp);

    const char req = deadKeyTakeRequeue();
    if (req != 0) pushProgramKey(req);
  }
}

bool inputConsumeBreakPending() {
  pumpProgramInput();
  const bool sawBreak = breakPending;
  breakPending = false;
  return sawBreak;
}

int inputProgramKeyCount() {
  pumpProgramInput();
  return (progHead + PROG_KEYS_SIZE - progTail) % PROG_KEYS_SIZE;
}

char inputReadProgramKey() {
  pumpProgramInput();
  if (progHead == progTail) return 0;
  const char c = progKeys[progTail];
  progTail = (uint8_t)((progTail + 1) % PROG_KEYS_SIZE);
  return c;
}

void inputDiscardPendingKeys() {
  while (!isQueueEmpty()) (void)dequeueKeyEvent();
}

void inputFlushProgramKeys() {
  progHead = progTail = 0;
  breakPending = false;
  deadKeyReset();
}

int processAllInput() {
  int processedCount = 0;
  while (!isQueueEmpty()) {
    KeyEvent event = dequeueKeyEvent();
    if (event.pressed) handleScreenEditorKey(event.keyCode, event.modifiers);
    processedCount++;
  }
  return processedCount;
}

bool dequeueKeyEventForCaller(uint8_t& keyCode, uint8_t& modifiers, bool& pressed) {
  if (isQueueEmpty()) return false;
  KeyEvent event = dequeueKeyEvent();
  keyCode = event.keyCode;
  modifiers = event.modifiers;
  pressed = event.pressed;
  return true;
}
