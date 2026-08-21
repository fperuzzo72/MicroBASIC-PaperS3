#pragma once

#include "config.h"

void inputSetup();
void enqueueKeyEvent(uint8_t keyCode, uint8_t modifiers, bool pressed);
int processAllInput();
char hidToAscii(uint8_t hid, uint8_t modifiers);

// Drains the whole input queue looking for a pending break request --
// Escape or Ctrl+C, the two classic BASIC "stop the program" gestures --
// discarding everything else along the way. Read by the runtime's checkch()
// (tb_runtime.cpp), which the interpreter polls after every statement, so a
// BLE-connected keyboard can abort a runaway program.
// BLE keystrokes are enqueued from the BLE host's own task context (see
// ble_keyboard.cpp), independent of loopTask, so they still arrive in the
// queue even while loopTask is blocked inside the interpreter. A
// physical Back press can't do this today: it's only ever turned into a
// queued event by processPhysicalButtons(), which runs from loop() and is
// itself blocked for the same reason RUN needed this escape hatch.
bool inputConsumeBreakPending();

// Keyboard for a RUNning program, behind the runtime's availch()/inch() and
// so behind BASIC's GET statement and its @A / @C special variables. Ordinary
// keystrokes that the break drain used to throw away are now queued here
// instead; arrow keys arrive as the MSX codes 28/29/30/31 (right/left/up/
// down). All three share one drain of the hardware queue, so calling any of
// them also collects any pending break.
int inputProgramKeyCount();   // keys waiting -- BASIC's @A
char inputReadProgramKey();   // next key, or 0 if none -- BASIC's GET / @C
void inputFlushProgramKeys(); // discard what was typed before the command ran

// Throw away every key still queued. For a screen that appears on its own --
// a prompt raised by something finishing, not by the user -- everything
// already in the queue was typed at a different screen and must not be
// allowed to answer a question that was not on display when it was pressed.
void inputDiscardPendingKeys();
