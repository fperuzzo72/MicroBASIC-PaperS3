#include "file_browser.h"

#include <Arduino.h>

#include <cstdio>
#include <cstring>

#include "input_handler.h"
#include "screen_editor.h"
#include "tb_bridge.h"

// Owned by input_handler.cpp; read by main.cpp's loop() to decide whether a
// repaint is due. Declared here the same way tb_bridge.cpp and wifi_sync.cpp
// do it rather than being added to a header.
extern bool screenDirty;

namespace {

bool active = false;
BrowserState state = BrowserState::MENU;
int selection = 0;
char statusText[80] = "";

// Menu entry order is the order they are drawn in, and the order below is the
// one the two collections read naturally in: browse, then create, for each.
enum MenuEntry {
  MENU_PROGRAMS = 0,
  MENU_NEW_PROGRAM,
  MENU_NOTES,
  MENU_NEW_NOTE,
};

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

// Not ported yet: both "New" entries and opening a note need the editor
// screen. Says so on the spot rather than appearing to work.
void notYet(const char* what) {
  snprintf(statusText, sizeof(statusText), "%s needs the editor screen, not ported yet.", what);
  screenDirty = true;
}

void chooseMenuEntry() {
  switch (selection) {
    case MENU_PROGRAMS: openCollection(FileCollection::PROGRAMS); return;
    case MENU_NOTES: openCollection(FileCollection::NOTES); return;
    case MENU_NEW_PROGRAM: notYet("Creating a program"); return;
    case MENU_NEW_NOTE: notYet("Creating a note"); return;
  }
}

void chooseFile() {
  if (selection < 0 || selection >= getFileCount()) return;
  const FileInfo& f = getFileList()[selection];

  if (getFileCollection() == FileCollection::PROGRAMS) {
    // Until the editor screen exists, choosing a program does the useful
    // thing that already works: hand it to the interpreter. LOAD takes the
    // filename exactly as listed, which is why programs are listed by
    // filename rather than by a prettified title (see file_manager.h).
    char line[MAX_FILENAME_LEN + 16];
    snprintf(line, sizeof(line), "LOAD \"%s\"", f.filename);
    browserStop();
    screenEditorStartNewInputLine();
    tbExecuteLine(line);
    if (screenEditorGetCursorCol() != 0) screenEditorStartNewInputLine();
    return;
  }

  notYet("Opening a note");
}

}  // namespace

const char* browserMenuLabel(int index) {
  switch (index) {
    case MENU_PROGRAMS: return "Programs";
    case MENU_NEW_PROGRAM: return "New program";
    case MENU_NOTES: return "Notes";
    case MENU_NEW_NOTE: return "New note";
    default: return "";
  }
}

void browserStart() {
  if (active) return;
  fileManagerSetup();
  active = true;
  state = BrowserState::MENU;
  selection = 0;
  statusText[0] = '\0';
  screenDirty = true;
}

void browserStop() {
  if (!active) return;
  active = false;
  screenDirty = true;
}

bool isBrowserActive() { return active; }
BrowserState getBrowserState() { return state; }
int getBrowserSelection() { return selection; }
const char* browserStatusText() { return statusText; }

void browserHandleKey(uint8_t keyCode, uint8_t modifiers) {
  (void)modifiers;
  if (!active) return;

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
      if (state == BrowserState::LIST) {
        state = BrowserState::MENU;
        selection = 0;
        statusText[0] = '\0';
        screenDirty = true;
      } else {
        browserStop();
      }
      return;

    default:
      return;
  }
}
