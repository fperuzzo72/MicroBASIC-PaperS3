#include "file_browser.h"

#include <Arduino.h>

#include <cstdio>
#include <cstring>

#include "dead_keys.h"
#include "input_handler.h"
#include "text_editor.h"
#if !MICROWRITER
#include "screen_editor.h"
#include "tb_bridge.h"
#endif

// Owned by input_handler.cpp; read by main.cpp's loop() to decide whether a
// repaint is due. Declared here the same way tb_bridge.cpp and wifi_sync.cpp
// do it rather than being added to a header.
extern bool screenDirty;

namespace {

bool active = false;
BrowserState state = BrowserState::MENU;
int selection = 0;
char statusText[80] = "";

// TITLE state
char titleBuffer[MAX_TITLE_LEN] = "";
int titleLen = 0;
bool titleIsForNewFile = false;

// VC mode: Enter loads through the interpreter instead of opening the editor,
// and Esc leaves for the terminal rather than backing up into the menu, which
// was never shown.
bool loadOnChoose = false;

// Auto-save bookkeeping. Both clocks are needed: idle catches the pause after
// a burst, the cap catches someone who never pauses.
unsigned long lastKeystrokeMs = 0;
unsigned long lastSaveMs = 0;
bool dirtySinceSave = false;

// Menu entry order is the order they are drawn in, and the order below is the
// one the two collections read naturally in: browse, then create, for each.
#if MICROWRITER
enum MenuEntry {
  MENU_NOTES = 0,
  MENU_NEW_NOTE,
  MENU_SYNC,
  MENU_READER,
};
#else
enum MenuEntry {
  MENU_PROGRAMS = 0,
  MENU_NEW_PROGRAM,
  MENU_NOTES,
  MENU_NEW_NOTE,
};
#endif

void openCollection(FileCollection c) {
  setFileCollection(c);  // switches and re-lists
  state = BrowserState::LIST;
  selection = 0;
  if (getFileCount() == 0) {
    snprintf(statusText, sizeof(statusText), "No files in %s.", fileCollectionName());
  } else {
    statusText[0] = '\0';
  }
  screenDirty = true;
}

void enterEditor() {
  state = BrowserState::EDIT;
  editorRecalculateLines();
  lastKeystrokeMs = millis();
  lastSaveMs = millis();
  dirtySinceSave = false;
  deadKeyReset();
  statusText[0] = '\0';
  screenDirty = true;
}

void openTitle(const char* current, bool forNewFile) {
  strncpy(titleBuffer, current, sizeof(titleBuffer) - 1);
  titleBuffer[sizeof(titleBuffer) - 1] = '\0';
  titleLen = (int)strlen(titleBuffer);
  titleIsForNewFile = forNewFile;
  state = BrowserState::TITLE;
  deadKeyReset();
  screenDirty = true;
}

void startNewFile(FileCollection c) {
  setFileCollection(c);
  createNewFile();
  // Straight to TITLE: a new file has no filename yet, and saveCurrentFile()
  // deliberately refuses to write one without a name, so typing first would
  // mean typing into something no auto-save could rescue.
  openTitle("Untitled", true);
}

void chooseMenuEntry() {
  switch (selection) {
    case MENU_NOTES: openCollection(FileCollection::NOTES); return;
    case MENU_NEW_NOTE: startNewFile(FileCollection::NOTES); return;
#if MICROWRITER
    case MENU_SYNC: startWifiSyncFromCommand(); return;
    case MENU_READER: startReaderSwitchFromCommand(); return;
#else
    case MENU_PROGRAMS: openCollection(FileCollection::PROGRAMS); return;
    case MENU_NEW_PROGRAM: startNewFile(FileCollection::PROGRAMS); return;
#endif
  }
}

void saveIfDirty() {
  if (!dirtySinceSave) return;
  if (editorGetCurrentFile()[0] == '\0') return;  // unnamed; TITLE handles it
  saveCurrentFile(false);
  dirtySinceSave = false;
  lastSaveMs = millis();
}

void chooseFile() {
  if (selection < 0 || selection >= getFileCount()) return;
  const FileInfo& f = getFileList()[selection];

#if !MICROWRITER
  if (loadOnChoose) {
    // Through the interpreter, not through file_manager's loadFile(): the
    // interpreter owns program memory, so loading anywhere else would leave
    // RUN and LIST looking at an empty program.
    char cmd[MAX_FILENAME_LEN + 16];
    snprintf(cmd, sizeof(cmd), "LOAD \"%s\"", f.filename);
    browserStop();
    screenEditorStartNewInputLine();
    if (tbExecuteLine(cmd)) {
      char msg[MAX_FILENAME_LEN + 16];
      snprintf(msg, sizeof(msg), "Loaded %s", f.filename);
      screenEditorTermPrintLine(msg);
    }
    // On failure the interpreter has already printed its own error, which
    // says more than anything this picker could.
    if (screenEditorGetCursorCol() != 0) screenEditorStartNewInputLine();
    return;
  }
#endif

  loadFile(f.filename);
  enterEditor();
}

}  // namespace

const char* browserMenuLabel(int index) {
  switch (index) {
    case MENU_NOTES: return "Notes";
    case MENU_NEW_NOTE: return "New note";
#if MICROWRITER
    case MENU_SYNC: return "Sync over WiFi";
    case MENU_READER: return "Switch to the reader";
#else
    case MENU_PROGRAMS: return "Programs";
    case MENU_NEW_PROGRAM: return "New program";
#endif
    default: return "";
  }
}

void browserStart() {
  if (active) return;
  fileManagerSetup();
  active = true;
  loadOnChoose = false;
  state = BrowserState::MENU;
  selection = 0;
  statusText[0] = '\0';
  screenDirty = true;
}

#if !MICROWRITER
void browserStartVc() {
  if (active) return;
  fileManagerSetup();
  active = true;
  loadOnChoose = true;
  setFileCollection(FileCollection::PROGRAMS);
  state = BrowserState::LIST;
  selection = 0;
  if (getFileCount() == 0) {
    snprintf(statusText, sizeof(statusText), "No files in %s.", fileCollectionName());
  } else {
    statusText[0] = '\0';
  }
  screenDirty = true;
}
#endif

void browserStop() {
  if (!active) return;
  active = false;
  screenDirty = true;
}

bool isBrowserActive() { return active; }
BrowserState getBrowserState() { return state; }
int getBrowserSelection() { return selection; }
const char* browserStatusText() { return statusText; }

void browserSetStatus(const char* text) {
  snprintf(statusText, sizeof(statusText), "%s", text ? text : "");
  screenDirty = true;
}

const char* browserTitleBuffer() { return titleBuffer; }

void browserLoop() {
  if (!active || state != BrowserState::EDIT || !dirtySinceSave) return;
  const unsigned long now = millis();
  if (now - lastKeystrokeMs >= AUTO_SAVE_IDLE_MS || now - lastSaveMs >= AUTO_SAVE_MAX_MS) {
    saveIfDirty();
  }
}

namespace {

void handleEditKey(uint8_t keyCode, uint8_t modifiers) {
  switch (keyCode) {
    case HID_KEY_ESCAPE:
      // Leaving is a save point, so nothing is lost by walking away even if
      // neither auto-save clock has fired yet.
      saveIfDirty();
      refreshFileList();  // a new file, or a retitled one, changes the listing
      state = BrowserState::LIST;
      selection = 0;
      screenDirty = true;
      return;

    case HID_KEY_LEFT: editorMoveCursorLeft(isShift(modifiers)); break;
    case HID_KEY_RIGHT: editorMoveCursorRight(isShift(modifiers)); break;
    case HID_KEY_UP: editorMoveCursorUp(isShift(modifiers)); break;
    case HID_KEY_DOWN: editorMoveCursorDown(isShift(modifiers)); break;
    case HID_KEY_HOME: editorMoveCursorHome(isShift(modifiers)); break;
    case HID_KEY_END: editorMoveCursorEnd(isShift(modifiers)); break;

    case HID_KEY_BACKSPACE:
      if (editorHasSelection()) editorDeleteSelection(); else editorDeleteChar();
      dirtySinceSave = true;
      break;

    case HID_KEY_ENTER:
      editorInsertChar('\n');
      dirtySinceSave = true;
      break;

    default: {
      // Ctrl shortcuts first, so Ctrl+C is a copy here rather than a 'c'.
      if (isCtrl(modifiers)) {
        switch (keyCode) {
          case 0x06: editorCopySelection(); break;                    // C
          case 0x1B: editorCutSelection(); dirtySinceSave = true; break;   // X
          case 0x19: editorPasteAtCursor(); dirtySinceSave = true; break;  // V
          case 0x04: editorSelectAll(); break;                        // A
          case 0x1D: editorUndo(); dirtySinceSave = true; break;      // Z
          case 0x16: saveIfDirty(); break;                            // S
          default: return;
        }
        break;
      }

      // Same US-International dead-key engine the terminal uses, so accented
      // input works identically in both.
      const char c = hidToAscii(keyCode, modifiers);
      if (c == 0) return;
      const char* composed = deadKeyProcess(c);
      if (composed == nullptr) {
        editorInsertChar(c);
      } else if (composed[0] != '\0') {
        editorInsertUtf8(composed);
        const char requeued = deadKeyTakeRequeue();
        if (requeued != 0) editorInsertChar(requeued);
      } else {
        return;  // dead key stored, nothing to show yet
      }
      dirtySinceSave = true;
      break;
    }
  }

  lastKeystrokeMs = millis();
  screenDirty = true;
}

void handleTitleKey(uint8_t keyCode, uint8_t modifiers) {
  if (keyCode == HID_KEY_ENTER) {
    if (titleLen > 0) {
      editorSetCurrentTitle(titleBuffer);
      if (editorGetCurrentFile()[0] == '\0') {
        char filename[MAX_FILENAME_LEN];
        deriveUniqueFilename(titleBuffer, filename, MAX_FILENAME_LEN);
        editorSetCurrentFile(filename);
      } else {
        updateFileTitle(editorGetCurrentFile(), titleBuffer);
      }
      editorSetUnsavedChanges(true);
      saveCurrentFile(false);
      dirtySinceSave = false;
      lastSaveMs = millis();
    }
    enterEditor();
    return;
  }

  if (keyCode == HID_KEY_ESCAPE) {
    deadKeyReset();
    if (titleIsForNewFile) {
      // Never named, so there is nothing on disk to go back to.
      state = BrowserState::MENU;
      selection = 0;
    } else {
      enterEditor();
    }
    screenDirty = true;
    return;
  }

  if (keyCode == HID_KEY_BACKSPACE) {
    if (titleLen > 0) titleBuffer[--titleLen] = '\0';
    screenDirty = true;
    return;
  }

  const char c = hidToAscii(keyCode, modifiers);
  if (c == 0 || c == '\n') return;
  if (titleLen < (int)sizeof(titleBuffer) - 1) {
    titleBuffer[titleLen++] = c;
    titleBuffer[titleLen] = '\0';
    screenDirty = true;
  }
}

}  // namespace

void browserHandleKey(uint8_t keyCode, uint8_t modifiers) {
  if (!active) return;

  if (state == BrowserState::EDIT) {
    handleEditKey(keyCode, modifiers);
    return;
  }
  if (state == BrowserState::TITLE) {
    handleTitleKey(keyCode, modifiers);
    return;
  }

  const int count = (state == BrowserState::MENU) ? BROWSER_MENU_COUNT : getFileCount();

  switch (keyCode) {
    case HID_KEY_UP:
      if (count > 0) selection = (selection - 1 + count) % count;
      screenDirty = true;
      return;

    case HID_KEY_DOWN:
      if (count > 0) selection = (selection + 1) % count;
      screenDirty = true;
      return;

    case HID_KEY_ENTER:
      if (state == BrowserState::MENU) {
        chooseMenuEntry();
      } else {
        chooseFile();
      }
      return;

    case HID_KEY_ESCAPE:
      // One level at a time, so backing out of a long list does not also
      // close the screen someone was still using.
      if (state == BrowserState::LIST && !loadOnChoose) {
        state = BrowserState::MENU;
        selection = 0;
        statusText[0] = '\0';
        screenDirty = true;
        return;
      }
#if MICROWRITER
      // The menu is the bottom of this machine: there is no terminal behind
      // it to return to, so closing here would leave a blank panel. Esc backs
      // out of a list and then stops.
      return;
#else
      browserStop();
      return;
#endif

    default:
      return;
  }
}
