#include "vc_browser.h"

#include "config.h"
#include "screen_editor.h"
#include "tb_bridge.h"

#include <SDCardManager.h>
#include <cstdio>
#include <cstring>

extern UIState currentState;
extern bool screenDirty;

static const char* PROGRAMS_DIR = "/MicroBASIC/programs";

// Reusing MAX_FILES (50) as the ceiling: same order of magnitude as the
// program store's own capacity, and the same limit the FILES command
// already imposes, so the two can't disagree about what's listable.
struct VcEntry {
  char name[MAX_FILENAME_LEN];
  unsigned long size;
};

static VcEntry entries[MAX_FILES];
static int entryCount = 0;
static int selected = 0;
static int scrollTop = 0;
static char statusText[64] = "";

// Rows given over to the title bar and the key-hint bar.
static constexpr int CHROME_ROWS = 2;

int vcFileCount() { return entryCount; }
int vcSelectedIndex() { return selected; }
int vcScrollTop() { return scrollTop; }
const char* vcStatusText() { return statusText; }

const char* vcFileName(int index) {
  if (index < 0 || index >= entryCount) return "";
  return entries[index].name;
}

unsigned long vcFileSize(int index) {
  if (index < 0 || index >= entryCount) return 0;
  return entries[index].size;
}

// One column per ~24 characters: a name plus a little breathing room. Gives
// 1 column on SCREEN 0 (32 cols), 2 on SCREEN 1/2 (48/64), 3 on SCREEN 3 (80).
int vcColumns() {
  const int c = screenEditorCols() / 24;
  return c < 1 ? 1 : c;
}

int vcColumnWidth() { return screenEditorCols() / vcColumns(); }

int vcRowsPerColumn() {
  const int r = screenEditorRows() - CHROME_ROWS;
  return r < 1 ? 1 : r;
}

int vcPageSize() { return vcColumns() * vcRowsPerColumn(); }

// Keeps the selection on screen, scrolling by whole pages the way a
// column-major lister has to (moving one row past the bottom of the last
// column means the next page starts, not that everything shifts up one).
static void ensureVisible() {
  const int page = vcPageSize();
  if (page < 1) return;
  if (selected < scrollTop) {
    scrollTop = (selected / page) * page;
  } else if (selected >= scrollTop + page) {
    scrollTop = (selected / page) * page;
  }
  if (scrollTop < 0) scrollTop = 0;
}

static void setHints() {
  snprintf(statusText, sizeof(statusText), "Enter:Load  Esc:Quit  %d file%s", entryCount,
           entryCount == 1 ? "" : "s");
}

void vcOpen() {
  entryCount = 0;
  selected = 0;
  scrollTop = 0;

  auto dir = SdMan.open(PROGRAMS_DIR);
  if (dir && dir.isDirectory()) {
    dir.rewindDirectory();
    char name[256];
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      file.getName(name, sizeof(name));
      // Same rule as the FILES command: everything except dot-files and
      // subdirectories, since SAVE stores under whatever name was typed
      // and doesn't force an extension.
      if (name[0] != '.' && !file.isDirectory() && entryCount < MAX_FILES) {
        strncpy(entries[entryCount].name, name, MAX_FILENAME_LEN - 1);
        entries[entryCount].name[MAX_FILENAME_LEN - 1] = '\0';
        entries[entryCount].size = (unsigned long)file.size();
        entryCount++;
      }
      file.close();
    }
  }
  if (dir) dir.close();

  setHints();
  currentState = UIState::VC_BROWSER;
  screenDirty = true;
}

void vcClose() {
  currentState = UIState::SCREEN_EDITOR;
  screenDirty = true;
}

static void moveSelection(int delta) {
  if (entryCount == 0) return;
  selected += delta;
  if (selected < 0) selected = 0;
  if (selected >= entryCount) selected = entryCount - 1;
  ensureVisible();
  setHints();
}

void vcHandleKey(uint8_t keyCode, uint8_t modifiers) {
  (void)modifiers;

  switch (keyCode) {
    case HID_KEY_ESCAPE:
      vcClose();
      return;

    case HID_KEY_UP:    moveSelection(-1); break;
    case HID_KEY_DOWN:  moveSelection(1);  break;
    // Left/right move by a whole column, which is what "next column" means
    // in a column-major layout -- matching how the list is actually drawn.
    case HID_KEY_LEFT:  moveSelection(-vcRowsPerColumn()); break;
    case HID_KEY_RIGHT: moveSelection(vcRowsPerColumn());  break;
    case HID_KEY_PAGE_UP:   moveSelection(-vcPageSize()); break;
    case HID_KEY_PAGE_DOWN: moveSelection(vcPageSize());  break;
    case HID_KEY_HOME:  moveSelection(-entryCount); break;
    case HID_KEY_END:   moveSelection(entryCount);  break;

    case HID_KEY_ENTER: {
      if (entryCount == 0) {
        vcClose();
        return;
      }
      // Load through the interpreter rather than through our own file
      // helper: the interpreter owns program memory now, so loading into
      // anything else would leave RUN/LIST looking at an empty program.
      char cmd[MAX_FILENAME_LEN + 16];
      snprintf(cmd, sizeof(cmd), "LOAD \"%s\"", entries[selected].name);
      const bool ok = tbExecuteLine(cmd);
      if (ok) {
        char msg[MAX_FILENAME_LEN + 16];
        snprintf(msg, sizeof(msg), "Loaded %s", entries[selected].name);
        screenEditorTermPrintLine(msg);
      }
      // Either way we return to the terminal: on failure the interpreter has
      // already printed its own error there, which is more informative than
      // anything this picker could put in its status bar.
      //
      // Only break the line if something left the cursor mid-row -- the same
      // rule the command dispatcher uses. TermPrintLine already ends with a
      // newline, so advancing here as well left a blank line behind.
      if (screenEditorGetCursorCol() != 0) screenEditorStartNewInputLine();
      vcClose();
      break;
    }

    default:
      return;  // nothing consumed, nothing to redraw
  }

  screenDirty = true;
}
