#pragma once

#include "config.h"
#include "file_manager.h"

// The EDITOR screen: a small menu, then a file list.
//
// Reached from the status bar's EDITOR button or by typing EDITOR. Four
// entries, which are the four things there are to do with the two collections
// (see file_manager.h for why notes and programs are genuinely different
// collections rather than one folder with two names):
//
//   Programs / New program   -- /MicroBASIC/programs, the same folder the
//                               interpreter's SAVE and LOAD use
//   Notes / New note         -- /notes, at the card's root, which is where
//                               MicroWriter has always kept them. Deliberately
//                               NOT under /MicroBASIC: the point is that one
//                               SD card can move between this device and a
//                               MicroWriter or an X4 MicroBASIC and every
//                               machine finds the same notes.
//
// This module owns the state and the key handling; main.cpp draws it, the same
// split wifi_sync.cpp already uses. Navigation is the inverted highlight bar
// the WiFi network list established: Up/Down move, Enter chooses, Esc backs
// out one level (list -> menu -> closed).
//
// Choosing a file opens it in the editor, both collections alike, which is
// what the X4 does: the browser is how you reach the editor, and LOAD is what
// you type at the BASIC prompt when you want the interpreter to have it.
//
// A new file has no filename until it has a title -- file_manager's
// saveCurrentFile() refuses an empty one on purpose -- so the two "New"
// entries go through TITLE before EDIT, and the name is derived from what is
// typed there (deriveUniqueFilename). Retitling an existing file renames it
// on disk to match.

enum class BrowserState {
  MENU,   // the four entries
  LIST,   // files in the chosen collection
  EDIT,   // the prose editor, on the file loaded into text_editor.cpp
  TITLE,  // naming a new file, or retitling an open one
};

// How many entries the menu has, and what they say. Kept here rather than in
// the drawing code so the two cannot disagree about the count.
// MicroWriter has no programs folder to browse: the interpreter that gives
// .bas files their meaning is not in that build at all, so offering them
// would be offering a dead end.
//
// MicroWriter's menu also carries SYNC and READER. MicroBASIC does not need
// them there: it has a prompt, and typing SYNC or READER reaches the same
// place. This machine has no prompt, so without menu entries those two would
// be touch-only, and the whole point of the keyboard is not having to reach
// for the screen. KBD is deliberately NOT among them -- asking for the
// on-screen keyboard already means you are touching the screen.
#if MICROWRITER
constexpr int BROWSER_MENU_COUNT = 4;
#else
constexpr int BROWSER_MENU_COUNT = 4;
#endif
const char* browserMenuLabel(int index);

void browserStart();  // opens at MENU; no-op if already open

// VC: opens straight into the programs list, and Enter there hands the file to
// the interpreter (LOAD) instead of the editor, returning to the terminal.
// Typed-only, like on the X4 -- it is a shortcut for loading a program, not a
// second way into the editor, so it does not get a status-bar button.
//
// The X4 draws this as a multi-column Volkov-Commander-style picker of its
// own (vc_browser.cpp plus ui_renderer.cpp's drawVcBrowser). That is not
// ported: it would mean a second list renderer to maintain beside this one,
// for a difference in looks rather than in what it does. The list here is
// the same one the browser already uses.
#if !MICROWRITER
void browserStartVc();
#endif
void browserStop();
bool isBrowserActive();

BrowserState getBrowserState();
int getBrowserSelection();       // index into the menu, or into the file list
const char* browserStatusText();  // one line under the list, may be empty

// Puts a message on that line from outside. On MicroWriter this is where
// anything the user needs to be told goes, since that build has no terminal
// to print into -- see main.cpp's notify().
void browserSetStatus(const char* text);

// TITLE state: the text being typed, for the drawing code.
const char* browserTitleBuffer();

// Drives auto-save; call every loop. Does nothing outside EDIT.
void browserLoop();

void browserHandleKey(uint8_t keyCode, uint8_t modifiers);
