#include "input_handler.h"
#include "text_editor.h"
#include "file_manager.h"
#include "ble_keyboard.h"
#include "wifi_sync.h"
#include "dead_keys.h"
#include "screen_editor.h"
#include "tb_bridge.h"
#include "vc_browser.h"

#include <Arduino.h>
#include <SDCardManager.h>
#include <Utf8.h>
#include <cctype>
#include <cstdlib>
#include <strings.h>

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
void storePairedDevice(const std::string& address, const std::string& name);
bool getStoredDevice(std::string& address, std::string& name);
void clearStoredDevice();
uint32_t getCurrentPasskey();
bool isDeviceScanning();
void refreshScanNow();
void clearAllBluetoothBonds();

// --- Input Queue ---
static KeyEvent inputQueue[INPUT_QUEUE_SIZE];
static int queueHead = 0;
static int queueTail = 0;
static volatile bool queueFull = false;

// --- CapsLock state ---
static bool capsLockOn = false;

// Where to return after title edit is confirmed or cancelled
static UIState renameReturnState = UIState::FILE_BROWSER;

// Forward declaration
static void openTitleEdit(const char* currentTitle, UIState returnTo);

// OTA app detection (defined in main.cpp)
extern OtaAppEntry otaApps[];
extern int otaAppCount;
void switchToOtaApp(int index);

// Orientation helper (defined in main.cpp) — see its own comment for why
// SCREEN_EDITOR uses this directly instead of going through
// currentOrientation.
void applyOrientationToRenderer(Orientation o);

// --- Shared UI state (defined in main.cpp) ---
extern UIState currentState;
extern int mainMenuSelection;
extern int selectedFileIndex;
extern int settingsSelection;
extern int bluetoothDeviceSelection;
extern int pairedKeyboardSelection;
extern Orientation currentOrientation;
extern bool screenDirty;
extern char renameBuffer[];
extern int renameBufferLen;

void inputSetup() {
  queueHead = 0;
  queueTail = 0;
  queueFull = false;
  capsLockOn = false;
}

static bool isQueueEmpty() {
  return (queueHead == queueTail) && !queueFull;
}

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

  // Letters a-z (HID 0x04-0x1D)
  if (hid >= 0x04 && hid <= 0x1D) {
    char base = 'a' + (hid - 0x04);
    return shifted ? (base - 32) : base;
  }

  // Number row (HID 0x1E-0x27)
  static const char unshifted[] = "1234567890";
  static const char shiftedNum[] = "!@#$%^&*()";
  if (hid >= 0x1E && hid <= 0x27) {
    int idx = hid - 0x1E;
    return isShift(modifiers) ? shiftedNum[idx] : unshifted[idx];
  }

  // Special keys
  switch (hid) {
    case 0x28: return '\n';  // Enter
    case 0x2B: return '\t';  // Tab
    case 0x2C: return ' ';   // Space

    // Symbol keys
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

// Handle text editor input
static void handleEditorKey(uint8_t keyCode, uint8_t modifiers) {
  // Ctrl shortcuts
  if (isCtrl(modifiers)) {
    if (keyCode == HID_KEY_S) {
      saveCurrentFile();
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_Z) {
      editorUndo();
      screenDirty = true;
      return;
    }
    // Clean mode saiu do Ctrl+Z para o Ctrl+L. O Ctrl+Z e desfazer em
    // qualquer editor, e um atalho que faz outra coisa nesse lugar surpreende
    // exatamente quando a pessoa mais precisa dele -- logo depois de apagar
    // algo por engano.
    if (keyCode == HID_KEY_L) {
      cleanMode = !cleanMode;
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_N) {
      openTitleEdit(editorGetCurrentTitle(), UIState::TEXT_EDITOR);
      return;
    }
    if (keyCode == HID_KEY_T) {
      writingMode = (writingMode == WritingMode::TYPEWRITER) ? WritingMode::NORMAL : WritingMode::TYPEWRITER;
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_F) {
      int v = static_cast<int>(fontSize);
      fontSize = static_cast<FontSize>((v + 1) % 3);
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_W) {
      showWordCount = !showWordCount;
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_P) {
      writingMode = (writingMode == WritingMode::PAGINATION) ? WritingMode::NORMAL : WritingMode::PAGINATION;
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_A) {
      editorSelectAll();
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_C) {
      editorCopySelection();
      return;
    }
    if (keyCode == HID_KEY_X) {
      editorCutSelection();
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_V) {
      editorPasteOverSelection();
      screenDirty = true;
      return;
    }
    // Ctrl+Left/Right: jump pages in pagination mode
    if (writingMode == WritingMode::PAGINATION) {
      int pageSize = editorGetStoredVisibleLines();
      if (keyCode == HID_KEY_LEFT) {
        for (int i = 0; i < pageSize; i++) editorMoveCursorUp();
        screenDirty = true;
        return;
      }
      if (keyCode == HID_KEY_RIGHT) {
        for (int i = 0; i < pageSize; i++) editorMoveCursorDown();
        screenDirty = true;
        return;
      }
    }
    return;
  }

  // ESC = save and return to file browser
  if (keyCode == HID_KEY_ESCAPE) {
    deadKeyReset();  // discard any pending dead key
    if (editorHasUnsavedChanges()) saveCurrentFile();
    currentState = UIState::FILE_BROWSER;
    screenDirty = true;
    return;
  }

  // Tab cycles writing modes
  if (keyCode == HID_KEY_TAB) {
    int v = static_cast<int>(writingMode);
    writingMode = static_cast<WritingMode>((v + 1) % 3);
    screenDirty = true;
    return;
  }

  // Navigation keys — Shift extends/keeps the selection, a plain move clears it
  {
    bool shift = isShift(modifiers);
    switch (keyCode) {
      case HID_KEY_LEFT:      editorMoveCursorLeft(shift);  screenDirty = true; return;
      case HID_KEY_RIGHT:     editorMoveCursorRight(shift); screenDirty = true; return;
      case HID_KEY_UP:        editorMoveCursorUp(shift);    screenDirty = true; return;
      case HID_KEY_DOWN:      editorMoveCursorDown(shift);  screenDirty = true; return;
      case HID_KEY_HOME:      editorMoveCursorHome(shift);  screenDirty = true; return;
      case HID_KEY_END:       editorMoveCursorEnd(shift);   screenDirty = true; return;
      case HID_KEY_BACKSPACE:
        if (editorHasSelection()) editorDeleteSelection(); else editorDeleteChar();
        screenDirty = true;
        return;
      case HID_KEY_DELETE:
        if (editorHasSelection()) editorDeleteSelection(); else editorDeleteForward();
        screenDirty = true;
        return;
      case HID_KEY_PAGE_UP:
      case HID_KEY_PAGE_DOWN: {
        // Typewriter mode only ever shows 1 line (editorSetVisibleLines(1)
        // in ui_renderer.cpp), so a "page" there has to mean something else
        // — editorGetPageJumpLines() is a real screenful either way.
        // Pagination mode's own linesPerPage IS editorGetStoredVisibleLines(),
        // so this doubles as an alias for its existing Ctrl+Left/Right jump.
        int jump = (writingMode == WritingMode::TYPEWRITER) ? editorGetPageJumpLines()
                                                              : editorGetStoredVisibleLines();
        if (jump < 1) jump = 1;
        for (int i = 0; i < jump; i++) {
          if (keyCode == HID_KEY_PAGE_UP) editorMoveCursorUp(shift);
          else editorMoveCursorDown(shift);
        }
        screenDirty = true;
        return;
      }
    }
  }

  // CapsLock toggle
  if (keyCode == HID_KEY_CAPSLOCK) {
    capsLockOn = !capsLockOn;
    return;
  }

  // Printable character — process through US-International dead key engine
  char c = hidToAscii(keyCode, modifiers);
  if (c != 0) {
    const char* composed = deadKeyProcess(c);
    if (composed == nullptr) {
      // No dead key involved: insert normally, replacing any selection
      if (editorHasSelection()) editorDeleteSelection();
      editorInsertChar(c);
      screenDirty = true;
    } else if (composed[0] != '\0') {
      // Composed result (or flushed dead key literal): insert UTF-8 string,
      // replacing any selection
      if (editorHasSelection()) editorDeleteSelection();
      editorInsertUtf8(composed);
      screenDirty = true;
      // If a non-composable char was requeued, insert it too
      char req = deadKeyTakeRequeue();
      if (req != 0) {
        editorInsertChar(req);
      }
    }
    // else: composed[0] == '\0' → dead key stored, nothing to insert yet
    // (leaves any existing selection untouched — nothing was typed)
  }
}

// --- Enter-key dispatch -----------------------------------------------------
// A logical line (screenEditorGetLogicalLineText() -- may span several
// physical rows if it wrapped while typing) goes almost entirely to the
// interpreter, which owns program storage and the classic commands:
//
//   "10 PRINT X"  -> its own tokenised program memory
//   LIST/RUN/NEW/SAVE/LOAD/CLS/CONT/DELETE/CATALOG/...  -> its own handling
//   anything else -> executed immediately in direct mode
//
// Only four things are intercepted, and only because they are this device's
// and not the language's: MENU/EXIT (leave the screen editor), VC (the program
// picker), SCREEN (the display modes) and FILES/DIR (aliases for the
// interpreter's own CATALOG). Everything the interpreter already
// implements is deliberately left to it -- routing it here instead would mean
// reimplementing, and diverging from, behaviour that is already correct.
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

  // EXIT is an alias, not a second command: on a machine with one way out of
  // the interpreter, both names should reach it.
  if (isWord("MENU") || isWord("EXIT")) {
    screenEditorClearLogicalLine();
    applyOrientationToRenderer(currentOrientation);
    currentState = UIState::MAIN_MENU;
    return;
  }
  if (isWord("VC")) {
    // Full-screen program picker (vc_browser.h). The typed "VC" is left on
    // screen on purpose: every other command now stays visible too, and it
    // gives the "Loaded ..." line that follows something to attach to. (It
    // used to be cleared, which made returning from VC look as though the
    // terminal had been wiped.)
    screenEditorStartNewInputLine();
    vcOpen();
    return;
  }
  if (isWord("FILES") || isWord("DIR")) {
    // Aliases: the interpreter calls its directory listing CATALOG. FILES is
    // the name this project used before the interpreter existed, DIR the one
    // anyone arriving from CP/M or DOS reaches for first. Both cost a word.
    screenEditorStartNewInputLine();
    tbExecuteLine("CATALOG");
    // Same rule as the interpreter path below: only break the line if it was
    // left mid-row. CATALOG ends with its own newline, so advancing here too
    // left a blank row that plain CATALOG didn't have.
    if (screenEditorGetCursorCol() != 0) screenEditorStartNewInputLine();
    return;
  }
  if (const char* arg = wordArg("SCREEN")) {
    // Ours, not the language's: the interpreter has no SCREEN, and these are
    // this project's own 32/48/64/80-column text modes (see README).
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
    // As above. Switching mode clears the screen and homes the cursor, so
    // advancing unconditionally here left row 0 blank and started on row 1.
    if (screenEditorGetCursorCol() != 0) screenEditorStartNewInputLine();
    return;
  }

  // Everything else -- including numbered lines, LIST, RUN, SAVE, LOAD, NEW --
  // is the interpreter's. Its own output (listings, errors, PRINT) reaches the
  // terminal through the runtime's outch().
  screenEditorStartNewInputLine();
  tbExecuteLine(p);
  // Only break the line if the interpreter left the cursor mid-row. Storing a
  // numbered line prints nothing at all, and PRINT ends with its own newline,
  // so unconditionally advancing here left a blank row after every entry --
  // typing "10 PRINT" then "20 GOTO 10" put an empty line between them.
  if (screenEditorGetCursorCol() != 0) screenEditorStartNewInputLine();
}

static void handleScreenEditorKey(uint8_t keyCode, uint8_t modifiers) {
  if (keyCode == HID_KEY_ESCAPE) {
    deadKeyReset();  // discard any pending dead key
    applyOrientationToRenderer(currentOrientation);  // restore the real setting
    currentState = UIState::MAIN_MENU;
    screenDirty = true;
    return;
  }

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

  // Printable character — same US-International dead key engine as the
  // prose editor, so accented input works here too.
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

// Open the title edit screen, returning to `returnTo` on confirm/cancel
static void openTitleEdit(const char* currentTitle, UIState returnTo) {
  strncpy(renameBuffer, currentTitle, MAX_TITLE_LEN - 1);
  renameBuffer[MAX_TITLE_LEN - 1] = '\0';
  renameBufferLen = strlen(renameBuffer);
  renameReturnState = returnTo;
  currentState = UIState::RENAME_FILE;
  screenDirty = true;
}

// Handle title edit input
static void handleRenameKey(uint8_t keyCode, uint8_t modifiers) {
  if (keyCode == HID_KEY_ENTER) {
    if (renameBufferLen > 0) {
      if (renameReturnState == UIState::TEXT_EDITOR) {
        editorSetCurrentTitle(renameBuffer);
        if (editorGetCurrentFile()[0] == '\0') {
          // New file — derive filename from title
          char filename[MAX_FILENAME_LEN];
          deriveUniqueFilename(renameBuffer, filename, MAX_FILENAME_LEN);
          editorSetCurrentFile(filename);
        } else {
          // Existing file — rename on disk to match new title
          updateFileTitle(editorGetCurrentFile(), renameBuffer);
        }
        editorSetUnsavedChanges(true);
        saveCurrentFile();
      } else {
        // Updating title of a file selected in the browser
        FileInfo* files = getFileList();
        updateFileTitle(files[selectedFileIndex].filename, renameBuffer);
      }
    }
    currentState = renameReturnState;
    screenDirty = true;
    return;
  }

  if (keyCode == HID_KEY_ESCAPE) {
    deadKeyReset();  // discard any pending dead key
    currentState = renameReturnState;
    screenDirty = true;
    return;
  }

  if (keyCode == HID_KEY_BACKSPACE) {
    deadKeyReset();  // dead key + backspace → discard dead key
    if (renameBufferLen > 0) {
      renameBufferLen--;
      renameBuffer[renameBufferLen] = '\0';
      screenDirty = true;
    }
    return;
  }

  // Allow all printable characters in a title (including spaces),
  // processed through the US-International dead key engine.
  char c = hidToAscii(keyCode, modifiers);
  if (c != 0 && c >= ' ') {
    const char* composed = deadKeyProcess(c);
    if (composed == nullptr) {
      // Normal character
      if (renameBufferLen < MAX_TITLE_LEN - 1) {
        renameBuffer[renameBufferLen++] = c;
        renameBuffer[renameBufferLen] = '\0';
        screenDirty = true;
      }
    } else if (composed[0] != '\0') {
      // Composed UTF-8 string (1–3 bytes typically): copy each byte
      for (const char* p = composed; *p != '\0' && renameBufferLen < MAX_TITLE_LEN - 1; p++) {
        renameBuffer[renameBufferLen++] = *p;
      }
      renameBuffer[renameBufferLen] = '\0';
      screenDirty = true;
      // If a non-composable char was requeued, insert it too
      char req = deadKeyTakeRequeue();
      if (req != 0 && req >= ' ' && renameBufferLen < MAX_TITLE_LEN - 1) {
        renameBuffer[renameBufferLen++] = req;
        renameBuffer[renameBufferLen] = '\0';
      }
    }
    // else: dead key stored, nothing to insert yet
  }
}

static void dispatchEvent(const KeyEvent& event) {
  if (!event.pressed) return;

  switch (currentState) {
    case UIState::MAIN_MENU: {
      // MicroBASIC, Browse Programs, New Program, Browse Files, New Note,
      // Settings, Sync -- keep in sync with ui_renderer.cpp's
      // baseMenuItems[] / BASE_MENU_COUNT.
      constexpr int BASE_MENU_COUNT = 7;
      int menuCount = BASE_MENU_COUNT + otaAppCount;
      if (event.keyCode == HID_KEY_DOWN) {
        mainMenuSelection = (mainMenuSelection + 1) % menuCount;
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_UP) {
        mainMenuSelection = (mainMenuSelection - 1 + menuCount) % menuCount;
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_ENTER) {
        if (mainMenuSelection == 0) {
          // MicroBASIC: the SCREEN 0-3 terminal -- see MicroBASIC repo's
          // docs/DEVELOPMENT_LOG.md. Only the terminal display resets;
          // whatever program the interpreter is holding (LOADed or typed
          // earlier this session) stays put.
          screenEditorReset();
          applyOrientationToRenderer(Orientation::LANDSCAPE_CCW);
          currentState = UIState::SCREEN_EDITOR;
          screenDirty = true;
        } else if (mainMenuSelection == 1 || mainMenuSelection == 3) {
          // Browse Programs / Browse Files. Same browser, same editor: the
          // only difference is which collection they are pointed at, and
          // that is decided here rather than inside either screen.
          setFileCollection(mainMenuSelection == 1 ? FileCollection::PROGRAMS
                                                   : FileCollection::NOTES);
          refreshFileList();
          selectedFileIndex = 0;
          currentState = UIState::FILE_BROWSER;
          screenDirty = true;
        } else if (mainMenuSelection == 2 || mainMenuSelection == 4) {
          // New Program / New Note -- the same prose editor, saving into
          // whichever folder was just selected. A program written here lands
          // in /MicroBASIC/programs, so LOAD finds it; a note lands in the
          // folder MicroWriter has always used.
          setFileCollection(mainMenuSelection == 2 ? FileCollection::PROGRAMS
                                                   : FileCollection::NOTES);
          createNewFile();
          // Programs are named by filename, so the prefill shows the shape
          // of one rather than a title -- the extension is the user's to
          // change, and seeing it there is what says so.
          openTitleEdit(getFileCollection() == FileCollection::PROGRAMS
                            ? "untitled.bas" : "Untitled",
                        UIState::TEXT_EDITOR);
        } else if (mainMenuSelection == 5) {
          currentState = UIState::SETTINGS;
          screenDirty = true;
        } else if (mainMenuSelection == 6) {
          wifiSyncStart();
          currentState = UIState::WIFI_SYNC;
          screenDirty = true;
        } else if (mainMenuSelection >= BASE_MENU_COUNT) {
          switchToOtaApp(mainMenuSelection - BASE_MENU_COUNT);
        }
      }
      break;
    }

    case UIState::FILE_BROWSER: {
      int fc = getFileCount();

      // Delete confirmation pending — Enter confirms, anything else cancels
      if (deleteConfirmPending) {
        if (event.keyCode == HID_KEY_ENTER && fc > 0) {
          FileInfo* files = getFileList();
          deleteFile(files[selectedFileIndex].filename);
          int newFc = getFileCount();
          if (selectedFileIndex >= newFc) selectedFileIndex = newFc - 1;
          if (selectedFileIndex < 0) selectedFileIndex = 0;
        }
        deleteConfirmPending = false;
        screenDirty = true;
        break;
      }

      if (event.keyCode == HID_KEY_DOWN && fc > 0) {
        selectedFileIndex = (selectedFileIndex + 1) % fc;
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_UP && fc > 0) {
        selectedFileIndex = (selectedFileIndex - 1 + fc) % fc;
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_ENTER && fc > 0) {
        FileInfo* files = getFileList();
        loadFile(files[selectedFileIndex].filename);
        screenDirty = true;
      } else if (isCtrl(event.modifiers) && event.keyCode == HID_KEY_N) {
        if (fc > 0) {
          FileInfo* files = getFileList();
          openTitleEdit(files[selectedFileIndex].title, UIState::FILE_BROWSER);
        }
      } else if (isCtrl(event.modifiers) && event.keyCode == HID_KEY_D) {
        if (fc > 0) {
          deleteConfirmPending = true;
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_ESCAPE) {
        currentState = UIState::MAIN_MENU;
        screenDirty = true;
      }
      break;
    }

    case UIState::TEXT_EDITOR:
      handleEditorKey(event.keyCode, event.modifiers);
      break;

    case UIState::SCREEN_EDITOR:
      handleScreenEditorKey(event.keyCode, event.modifiers);
      break;

    case UIState::VC_BROWSER:
      vcHandleKey(event.keyCode, event.modifiers);
      break;

    case UIState::RENAME_FILE:
      handleRenameKey(event.keyCode, event.modifiers);
      break;

    case UIState::SETTINGS: {
      const int SETTINGS_COUNT = 7;  // Orientation, Dark Mode, Writing Mode, Font Size, Game Buttons, Bluetooth, Paired Keyboards

      // Up/Down: navigate settings list (physical buttons also map here)
      if (event.keyCode == HID_KEY_DOWN) {
        settingsSelection = (settingsSelection + 1) % SETTINGS_COUNT;
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_UP) {
        settingsSelection = (settingsSelection - 1 + SETTINGS_COUNT) % SETTINGS_COUNT;
        screenDirty = true;

      // Enter or Right: cycle setting forward
      } else if (event.keyCode == HID_KEY_ENTER || event.keyCode == HID_KEY_RIGHT) {
        if (settingsSelection == 0) {
          int v = static_cast<int>(currentOrientation);
          currentOrientation = static_cast<Orientation>((v + 1) % 4);
        } else if (settingsSelection == 1) {
          darkMode = !darkMode;
        } else if (settingsSelection == 2) {
          int v = static_cast<int>(writingMode);
          writingMode = static_cast<WritingMode>((v + 1) % 3);
        } else if (settingsSelection == 3) {
          int v = static_cast<int>(fontSize);
          fontSize = static_cast<FontSize>((v + 1) % 3);
        } else if (settingsSelection == 4) {
          remapButtonsInPrograms = !remapButtonsInPrograms;
        } else if (settingsSelection == 5) {
          currentState = UIState::BLUETOOTH_SETTINGS;
        } else if (settingsSelection == 6) {
          pairedKeyboardSelection = 0;
          currentState = UIState::PAIRED_KEYBOARDS;
        }
        screenDirty = true;

      // Left: cycle setting backward (keyboard only — physical L/R map to Up/Down)
      } else if (event.keyCode == HID_KEY_LEFT) {
        if (settingsSelection == 0) {
          int v = static_cast<int>(currentOrientation);
          currentOrientation = static_cast<Orientation>((v - 1 + 4) % 4);
        } else if (settingsSelection == 1) {
          darkMode = !darkMode;
        } else if (settingsSelection == 2) {
          int v = static_cast<int>(writingMode);
          writingMode = static_cast<WritingMode>((v - 1 + 3) % 3);
        } else if (settingsSelection == 3) {
          int v = static_cast<int>(fontSize);
          fontSize = static_cast<FontSize>((v - 1 + 3) % 3);
        } else if (settingsSelection == 4) {
          remapButtonsInPrograms = !remapButtonsInPrograms;
        }
        screenDirty = true;

      } else if (event.keyCode == HID_KEY_ESCAPE) {
        currentState = UIState::MAIN_MENU;
        screenDirty = true;
      }
      break;
    }

    case UIState::BLUETOOTH_SETTINGS: {
      int deviceCount = getDiscoveredDeviceCount();

      // Ensure selection is within bounds
      if (bluetoothDeviceSelection >= deviceCount && deviceCount > 0) {
        bluetoothDeviceSelection = deviceCount - 1;
      } else if (deviceCount == 0) {
        bluetoothDeviceSelection = 0; // Reset to 0 when no devices
      }

      if (event.keyCode == HID_KEY_ESCAPE) {
        DBG_PRINTLN("[INPUT] BT: Escape pressed - returning to settings");
        currentState = UIState::SETTINGS;
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_DOWN) {
        if (deviceCount > 0) {
          bluetoothDeviceSelection = (bluetoothDeviceSelection + 1) % deviceCount;
          DBG_PRINTF("[INPUT] BT: Down pressed - selection now %d/%d\n", bluetoothDeviceSelection, deviceCount);
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_UP) {
        if (deviceCount > 0) {
          bluetoothDeviceSelection = (bluetoothDeviceSelection - 1 + deviceCount) % deviceCount;
          DBG_PRINTF("[INPUT] BT: Up pressed - selection now %d/%d\n", bluetoothDeviceSelection, deviceCount);
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_ENTER) {
        if (deviceCount > 0 && !isDeviceScanning()) {
          // Connect to the selected device
          connectToDevice(bluetoothDeviceSelection);
        } else if (!isDeviceScanning()) {
          // No devices — start a new scan
          startDeviceScan();
        }
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_RIGHT) {
        // Right button = re-scan for devices
        if (!isDeviceScanning()) {
          startDeviceScan();
        }
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_LEFT) {
        if (isKeyboardConnected()) {
          disconnectCurrentDevice();
          screenDirty = true;
        }
      }
      break;
    }

    case UIState::PAIRED_KEYBOARDS: {
      int count = getPairedKeyboardCount();

      if (event.keyCode == HID_KEY_ESCAPE) {
        currentState = UIState::SETTINGS;
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_DOWN) {
        if (count > 0) {
          pairedKeyboardSelection = (pairedKeyboardSelection + 1) % count;
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_UP) {
        if (count > 0) {
          pairedKeyboardSelection = (pairedKeyboardSelection - 1 + count) % count;
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_ENTER) {
        if (count > 0) {
          connectToPairedKeyboard(pairedKeyboardSelection);
          currentState = UIState::SETTINGS;
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_D) {
        if (count > 0) {
          removePairedKeyboard(pairedKeyboardSelection);
          int newCount = getPairedKeyboardCount();
          if (pairedKeyboardSelection >= newCount && newCount > 0)
            pairedKeyboardSelection = newCount - 1;
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_LEFT) {
        // Disconnect if this keyboard is the currently connected one
        if (count > 0 && isKeyboardConnected()) {
          std::string addr, name; uint8_t addrType;
          getPairedKeyboard(pairedKeyboardSelection, addr, name, addrType);
          if (getCurrentDeviceAddress() == addr) {
            disconnectCurrentDevice();
            screenDirty = true;
          }
        }
      }
      break;
    }

    case UIState::WIFI_SYNC: {
      syncHandleKey(event.keyCode, event.modifiers);
      break;
    }

    default:
      break;
  }
}

// --- Keyboard for a RUNning program -------------------------------------
//
// While a program runs, loopTask is inside the interpreter and nothing calls
// processAllInput(), so the normal editor dispatch is not running. The queue
// still fills, though (BLE delivers from its own task), and the interpreter
// polls the runtime after every statement. These few functions are what that
// poll drains into: break requests on one side, ordinary keystrokes on the
// other, so a program can both be stopped AND read the keyboard.
//
// The ring is small on purpose. A game wants the key that is being held down
// now, not a backlog of everything pressed while it was busy repainting, so
// when it fills the newest key is dropped rather than the oldest read.

static char progKeys[16];
static uint8_t progHead = 0;
static uint8_t progTail = 0;
static bool breakPending = false;

static constexpr uint8_t PROG_KEYS_SIZE = (uint8_t)sizeof(progKeys);

// One character into the ring. Characters are *bytes*, and an accented one
// is a single byte in Latin-1 rather than the two UTF-8 bytes the editors
// pass around. That is not a shortcut, it is what makes BASIC strings work:
// they are byte arrays, so LEN("ola" with an accent) has to be 3 and MID$
// has to index characters. Storing UTF-8 would silently break both, and
// one byte per character is what the machines this imitates actually did.
// tb_runtime.cpp's outch() converts back to UTF-8 on the way to the screen.
static void pushProgramKey(char c) {
  const uint8_t next = (uint8_t)((progHead + 1) % PROG_KEYS_SIZE);
  if (next == progTail) return;  // full: drop the newest, see above
  progKeys[progHead] = c;
  progHead = next;
}

// Both inputConsumeBreakPending() and the GET path call this, because the
// interpreter reaches them at different moments and whichever runs first has
// to be the one that empties the hardware queue.
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
      // all almost certainly wants them. These are the MSX codes, which is
      // the machine this environment's SCREEN modes and editing model follow.
      switch (event.keyCode) {
        case HID_KEY_RIGHT: c = 28; break;
        case HID_KEY_LEFT:  c = 29; break;
        case HID_KEY_UP:    c = 30; break;
        case HID_KEY_DOWN:  c = 31; break;
        // ASCII backspace. Needed by INPUT, which does its own line editing
        // inside the interpreter (tb_runtime.cpp's consins) and has no other
        // way to hear about it; a program using GET sees it too, correctly.
        case HID_KEY_BACKSPACE: c = 8; break;
        default: continue;
      }
      pushProgramKey(c);
      continue;
    }

    // The same US-International dead key engine the editors use, so INPUT
    // takes accents too. What comes back is UTF-8; what goes into the ring
    // is one byte, see pushProgramKey.
    const char* composed = deadKeyProcess(c);
    if (composed == nullptr) {
      pushProgramKey(c);  // not part of any composition
      continue;
    }
    if (composed[0] == '\0') continue;  // dead key held, nothing typed yet

    const unsigned char* u = (const unsigned char*)composed;
    const uint32_t cp = utf8NextCodepoint(&u);
    if (cp == 0) continue;
    // Above Latin-1 there is no single byte to store, and BASIC strings are
    // byte arrays. Nothing the US-International layout composes lands up
    // here, so dropping is not a loss the user can reach.
    if (cp <= 0xFF) pushProgramKey((char)(unsigned char)cp);

    // A dead key followed by a character it can't combine with emits the
    // accent literally and hands the character back to be typed on its own.
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

// Called before each command is handed to the interpreter, so that whatever
// was typed *before* RUN isn't waiting to be read as the first move of the
// game that RUN starts.
void inputDiscardPendingKeys() {
  while (!isQueueEmpty()) (void)dequeueKeyEvent();
}

void inputFlushProgramKeys() {
  progHead = progTail = 0;
  breakPending = false;
  // A dead key held down when the last command ended is not an accent on the
  // first character of the next one.
  deadKeyReset();
}

int processAllInput() {
  int processedCount = 0;
  while (!isQueueEmpty()) {
    KeyEvent event = dequeueKeyEvent();
    dispatchEvent(event);
    processedCount++;
  }
  return processedCount;
}
