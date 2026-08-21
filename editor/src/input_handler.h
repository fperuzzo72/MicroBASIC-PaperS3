#pragma once

#include "config.h"

// Ported from MicroBASIC's own input_handler.cpp (editor/port-staging/),
// trimmed to what this bring-up needs: dispatchEvent() there routes by
// UIState across the main menu, file browser, prose text editor, BLE
// pairing screen, etc. -- none of which exist here yet. This version's
// processAllInput() routes every key straight to the screen editor, since
// that's the only "screen" this build has. Re-expand dispatchEvent() into
// something UIState-driven again once the menu/file-browser/prose-editor
// systems get ported.
//
// The program-key ring buffer, break detection, and the queue itself
// (enqueueKeyEvent/dequeueKeyEvent) are carried over unchanged -- BASIC's
// GET/@A/@C and the interpreter's checkch()/consins() need exactly the same
// contract regardless of what else exists around them.

void inputSetup();
void enqueueKeyEvent(uint8_t keyCode, uint8_t modifiers, bool pressed);
int processAllInput();
char hidToAscii(uint8_t hid, uint8_t modifiers);

// Drains the whole input queue looking for a pending break request --
// Escape or Ctrl+C -- discarding everything else along the way. Read by the
// runtime's checkch() (tb_runtime.cpp), which the interpreter polls after
// every statement, so a break can interrupt a running program.
bool inputConsumeBreakPending();

// Keyboard for a RUNning program, behind the runtime's availch()/inch() and
// so behind BASIC's GET statement and its @A / @C special variables. All
// three share one drain of the hardware queue, so calling any of them also
// collects any pending break. Arrow keys arrive as the MSX codes 28/29/30/31
// (right/left/up/down).
int inputProgramKeyCount();   // keys waiting -- BASIC's @A
char inputReadProgramKey();   // next key, or 0 if none -- BASIC's GET / @C
void inputFlushProgramKeys(); // discard what was typed before the command ran

// Throw away every key still queued -- called after a command finishes
// running, so a keystroke made while it was busy doesn't become the first
// move of whatever runs next.
void inputDiscardPendingKeys();
