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

// Pops one already-queued key event for a caller that wants to route it
// somewhere other than the screen editor -- main.cpp uses this to hand
// input to wifi_sync.cpp's syncHandleKey() instead of processAllInput()
// while the WiFi setup screen is up, without wifi_sync.cpp or this header
// needing to know about each other. Unlike processAllInput() (which only
// acts on press events), this hands back every event including releases,
// since what "not pressed" means is the caller's call, not this queue's.
// Returns false once the queue is empty.
bool dequeueKeyEventForCaller(uint8_t& keyCode, uint8_t& modifiers, bool& pressed);

// Starts the WiFi file-transfer flow, the same way tapping the status bar's
// SYNC button does (forcing the on-screen keyboard visible first, so a
// physical/BLE-only typist isn't stranded if BLE gets suspended mid-connect
// -- see wifi_sync.cpp's suspendBleForWifiConnect()). Implemented in
// main.cpp, which owns the on-screen-keyboard-visible flag; called from
// input_handler.cpp's executeLogicalLine() so typing SYNC reaches the same
// entry point as the button. A no-op if the flow is already running.
void startWifiSyncFromCommand();

// Opens the READER confirmation, the same way tapping the status bar's READER
// button does. Implemented in main.cpp, which owns the modal; called from
// executeLogicalLine() so the dual-boot switch is reachable without touching
// the screen, same as SYNC. A no-op if there is no sibling app to switch to.
void startReaderSwitchFromCommand();

// Opens the EDITOR screen (file_browser.h), the same way tapping the status
// bar's EDITOR button does. Implemented in main.cpp, which owns the
// on-screen-keyboard-visible flag; the list is arrow-driven, so the keyboard
// is forced visible for the same reason SYNC does it.
void startEditorFromCommand();

// VC: the programs list with LOAD on Enter (file_browser.h's browserStartVc).
// Typed-only, no status-bar button.
void startVcFromCommand();

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
