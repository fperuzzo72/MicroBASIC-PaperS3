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
// The two "New" entries, and opening a note, need the editor screen, which is
// not ported yet -- they say so rather than doing nothing.

enum class BrowserState {
  MENU,  // the four entries
  LIST,  // files in the chosen collection
};

// How many entries the menu has, and what they say. Kept here rather than in
// the drawing code so the two cannot disagree about the count.
constexpr int BROWSER_MENU_COUNT = 4;
const char* browserMenuLabel(int index);

void browserStart();  // opens at MENU; no-op if already open
void browserStop();
bool isBrowserActive();

BrowserState getBrowserState();
int getBrowserSelection();       // index into the menu, or into the file list
const char* browserStatusText();  // one line under the list, may be empty

void browserHandleKey(uint8_t keyCode, uint8_t modifiers);
