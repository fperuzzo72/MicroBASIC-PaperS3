#include "input_handler.h"

#include <Arduino.h>

#include <cctype>

// Key plumbing only: the hardware queue, and the HID-to-character mapping both
// machines need. What the MicroBASIC terminal does with a key -- line editing,
// the command words, the byte ring BASIC's GET reads -- lives in
// terminal_input.cpp, which the MicroWriter build leaves out entirely along
// with the interpreter it exists to feed. See platformio.ini's [env:microwriter].

// Read by main.cpp's loop() to decide whether a redraw is due, and set by
// anything that changes what is on screen: the terminal (terminal_input.cpp),
// the interpreter (tb_bridge.cpp/tb_runtime.cpp), the file browser and the
// WiFi wizard. Lives here because this is the one file every build compiles.
bool screenDirty = true;

// --- Input Queue -------------------------------------------------------
static KeyEvent inputQueue[INPUT_QUEUE_SIZE];
static int queueHead = 0;
static int queueTail = 0;
static volatile bool queueFull = false;

static bool capsLockOn = false;  // reserved: nothing currently sets this true

void inputSetup() {
  queueHead = 0;
  queueTail = 0;
  queueFull = false;
  capsLockOn = false;
}

bool inputQueueIsEmpty() { return (queueHead == queueTail) && !queueFull; }

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

KeyEvent inputDequeueKeyEvent() {
  KeyEvent event = {0, 0, false};
  noInterrupts();
  if (!inputQueueIsEmpty()) {
    event = inputQueue[queueTail];
    queueTail = (queueTail + 1) % INPUT_QUEUE_SIZE;
    queueFull = false;
  }
  interrupts();
  return event;
}

char hidToAscii(uint8_t hid, uint8_t modifiers) {
  bool shifted = isShift(modifiers) ^ capsLockOn;

  if (hid >= 0x04 && hid <= 0x1D) {
    char base = 'a' + (hid - 0x04);
    return shifted ? (base - 32) : base;
  }

  static const char unshifted[] = "1234567890";
  static const char shiftedNum[] = "!@#$%^&*()";
  if (hid >= 0x1E && hid <= 0x27) {
    int idx = hid - 0x1E;
    return isShift(modifiers) ? shiftedNum[idx] : unshifted[idx];
  }

  switch (hid) {
    case 0x28: return '\n';  // Enter
    case 0x2B: return '\t';  // Tab
    case 0x2C: return ' ';   // Space

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


// Throw away whatever is still queued. Only touches the queue, so it lives
// here rather than with the terminal: the WiFi wizard and the file browser
// both use it to keep a keystroke meant for the previous screen from landing
// on the one that just opened.
void inputDiscardPendingKeys() {
  while (!inputQueueIsEmpty()) (void)inputDequeueKeyEvent();
}

bool dequeueKeyEventForCaller(uint8_t& keyCode, uint8_t& modifiers, bool& pressed) {
  if (inputQueueIsEmpty()) return false;
  KeyEvent event = inputDequeueKeyEvent();
  keyCode = event.keyCode;
  modifiers = event.modifiers;
  pressed = event.pressed;
  return true;
}
