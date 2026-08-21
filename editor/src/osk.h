#pragma once

#include <cstdint>

class GfxRenderer;

// osk.h -- on-screen (touch) keyboard, standalone/self-contained.
//
// Not on the original feature list, but not optional either: the PaperS3 has
// no physical buttons at all (unlike the X4's d-pad fallback), so without
// this the device is mute the moment no BLE keyboard is paired. It injects
// standard USB HID keycodes + a standard HID modifier bitmask through a
// caller-supplied callback -- the SAME wire format
// input_handler.cpp::enqueueKeyEvent() already expects on the X4 -- so the
// editor, dead-key composition (dead_keys.h operates on the *character* that
// hidToAscii() derives, not on HID codes) and the BASIC layer need no changes
// to accept touch once that file is ported. See README's "on-screen
// keyboard" note.
//
// input_handler.cpp itself isn't ported yet (it pulls in the whole
// not-yet-ported editor/BLE/wifi stack), so for now this is demonstrated
// standalone in the bring-up program via its own callback, echoing typed
// characters into the terminal grid -- see oskHidToChar()'s doc comment for
// the one piece of state that duplicates input_handler.cpp and must be kept
// in sync until that file is ported and this can just call the real thing.

// Standard USB HID modifier bitmask (matches config.h's MOD_* on the X4):
// bit0 LCtrl, bit1 LShift, bit2 LAlt, bit4 RCtrl, bit5 RShift, bit6 RAlt.
static constexpr uint8_t OSK_MOD_CTRL_LEFT = 0x01;
static constexpr uint8_t OSK_MOD_SHIFT_LEFT = 0x02;
static constexpr uint8_t OSK_MOD_ALT_LEFT = 0x04;

// Keys a caller's callback typically needs to special-case rather than read
// as a character (oskHidToChar() returns 0 for all of these except Enter,
// which it maps to '\n').
static constexpr uint8_t OSK_HID_ESCAPE = 0x29;
static constexpr uint8_t OSK_HID_BACKSPACE = 0x2A;
static constexpr uint8_t OSK_HID_LEFT = 0x50;
static constexpr uint8_t OSK_HID_RIGHT = 0x4F;

// Called once per tapped key: a HID usage ID (e.g. 0x04 for 'A') and the
// modifier byte active for that tap. The keyboard itself decides when Shift/
// Ctrl are armed (see "one-shot modifiers" below) -- callers just receive
// the fully-resolved (keycode, modifiers) pair, exactly one call per tap.
using OskKeyCallback = void (*)(uint8_t hidCode, uint8_t modifiers);

// Geometry: draws into the caller-specified rectangle (the "keyboard area"
// below the terminal in main.cpp's layout) via the caller's own renderer/
// fonts -- osk.cpp deliberately doesn't own a GfxRenderer or know about
// main.cpp's font IDs, so it stays reusable once the real editor is ported.
// labelFontId draws each key's main character; smallLabelFontId (meant to be
// a smaller size, e.g. Ubuntu 10 vs NotoSans 12) draws the small corner hint
// on digit/symbol keys showing what Shift produces -- see oskDraw()'s
// comment. Must be called once before the keyboard is used.
void oskInit(GfxRenderer& renderer, int labelFontId, int smallLabelFontId, int x, int y, int width,
             int height, OskKeyCallback onKey);

// Draws every key (outline + centered label) into the current framebuffer.
// Does not call displayBuffer() -- the caller controls the refresh, same
// convention as everything else in this bring-up program.
void oskDraw();

// Hit-tests a LOGICAL tap position (already passed through
// GfxRenderer::tapToLogical()) against the key grid. Returns true if the tap
// landed on a key (and fires the callback / updates Shift-Ctrl state);
// false if the tap was outside the keyboard area entirely, so the caller
// can fall through to handling taps elsewhere on screen.
//
// Shift and Ctrl are ONE-SHOT: tapping either arms it (drawn inverted) for
// exactly the next non-modifier key, then it clears itself automatically --
// the standard "temporary shift" a touch keyboard uses instead of a held
// key. Caps Lock is a separate, genuinely sticky toggle (stays on until
// tapped again), matching a real keyboard's Caps Lock key.
bool oskHandleTap(int logicalX, int logicalY);

// True while Shift or Ctrl is armed / Caps Lock is on -- lets the caller
// redraw the keyboard (oskDraw()) so the armed key shows inverted.
bool oskShiftArmed();
bool oskCtrlArmed();
bool oskCapsLockOn();

// Renders the character hidCode+modifiers would produce, using the exact
// same rules as input_handler.cpp's hidToAscii() (letters XOR Caps Lock with
// Shift; digits/symbols use Shift alone; Caps Lock has no effect on them).
// Duplicated here rather than shared because input_handler.cpp isn't ported
// yet -- see this header's top comment. Returns 0 for keys with no visible
// character (arrows, Enter, Tab, Backspace, the modifiers themselves).
char oskHidToChar(uint8_t hidCode, uint8_t modifiers);
