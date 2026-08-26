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

// Everything the MicroBASIC terminal does with a keystroke: line editing at
// the prompt, this project's own command words, and the byte ring the
// interpreter's GET/@A/@C read from. Split out of input_handler.cpp so the
// MicroWriter build can leave it out with the interpreter -- that machine has
// no prompt to type at. The queue itself, dead keys and hidToAscii stay
// shared; see input_handler.cpp.

extern bool screenDirty;

// The queue's own accessors, shared with input_handler.cpp.
bool inputQueueIsEmpty();
KeyEvent inputDequeueKeyEvent();

// --- Screen editor: the only "screen" this build has --------------------
//
// executeLogicalLine()/handleScreenEditorKey() are ported from the X4's
// input_handler.cpp almost unchanged. MENU/EXIT are deliberately absent:
// the X4 needed a menu because it had nowhere else to put navigation, and
// here the status bar covers all of it.

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

  if (isWord("SYNC")) {
    // Same entry point as tapping the status bar's SYNC button -- lets
    // someone start the WiFi transfer flow without touching the screen.
    screenEditorStartNewInputLine();
    startWifiSyncFromCommand();
    return;
  }
  if (isWord("READER")) {
    // Same entry point as the status bar's READER button, confirmation and
    // all -- typing it is not treated as its own confirmation.
    screenEditorStartNewInputLine();
    startReaderSwitchFromCommand();
    return;
  }
  if (isWord("VC")) {
    // The typed "VC" is left on screen on purpose, so the "Loaded ..." line
    // that follows has something to attach to.
    screenEditorStartNewInputLine();
    startVcFromCommand();
    return;
  }
  if (isWord("EDITOR")) {
    // Same entry point as the status bar's EDITOR button.
    screenEditorStartNewInputLine();
    startEditorFromCommand();
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
  // Escape returned to the main menu on the X4. There is no menu here (see
  // above), so it falls through as a no-op: it is not a navigation key below,
  // and hidToAscii() returns 0 for it, so nothing is inserted either. A
  // running program's own Escape-to-break still works -- that is
  // pumpProgramInput()'s path, not this one.

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
  while (!inputQueueIsEmpty()) {
    KeyEvent event = inputDequeueKeyEvent();
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

void inputFlushProgramKeys() {
  progHead = progTail = 0;
  breakPending = false;
  deadKeyReset();
}

int processAllInput() {
  int processedCount = 0;
  while (!inputQueueIsEmpty()) {
    KeyEvent event = inputDequeueKeyEvent();
    if (event.pressed) handleScreenEditorKey(event.keyCode, event.modifiers);
    processedCount++;
  }
  return processedCount;
}

