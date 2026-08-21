#include "osk.h"

#include <GfxRenderer.h>

#include <cstdio>
#include <cstring>

namespace {

enum class KeyKind : uint8_t { Normal, Shift, Ctrl, CapsLock };

struct KeyDef {
  uint8_t hid;      // USB HID usage ID; 0 for modifier-only keys (kind != Normal)
  uint8_t units;    // width in grid units (see kUnitsPerRow below)
  uint8_t rowSpan;  // height in rows; 2 for Enter (see kRow2's comment)
  KeyKind kind;
  const char* label;  // fixed label; nullptr means "derive from hid via oskHidToChar()"
};

// USB HID keyboard usage IDs this layout needs. Letters (0x04-0x1D, A-Z
// alphabetical) and digits (0x1E-0x27, 1-9 then 0) are generated in the row
// tables below rather than named individually.
constexpr uint8_t HID_ENTER = 0x28;
constexpr uint8_t HID_BACKSPACE = OSK_HID_BACKSPACE;
constexpr uint8_t HID_TAB = 0x2B;
constexpr uint8_t HID_SPACE = 0x2C;
constexpr uint8_t HID_MINUS = 0x2D;
constexpr uint8_t HID_EQUALS = 0x2E;
constexpr uint8_t HID_LBRACKET = 0x2F;
constexpr uint8_t HID_RBRACKET = 0x30;
constexpr uint8_t HID_BACKSLASH = 0x31;
constexpr uint8_t HID_SEMICOLON = 0x33;
constexpr uint8_t HID_APOSTROPHE = 0x34;
constexpr uint8_t HID_GRAVE = 0x35;
constexpr uint8_t HID_COMMA = 0x36;
constexpr uint8_t HID_PERIOD = 0x37;
constexpr uint8_t HID_SLASH = 0x38;
constexpr uint8_t HID_RIGHT = OSK_HID_RIGHT;
constexpr uint8_t HID_LEFT = OSK_HID_LEFT;
constexpr uint8_t HID_UP = 0x52;
constexpr uint8_t HID_DOWN = 0x51;

// 16 units/row. With the 960-wide landscape overlay this is 60px/unit --
// close to a real keycap's ~60px (6.5mm at this panel's 234ppi) pitch, and
// a proper ANSI-ish shape now fits: both Shifts, brackets, backslash,
// minus/equals, and Enter in its conventional end-of-home-row position.
constexpr int kUnitsPerRow = 16;

// Row 0: digits, - =, Backspace.
const KeyDef kRow0[] = {
    {0x1E, 1, 1, KeyKind::Normal, nullptr}, {0x1F, 1, 1, KeyKind::Normal, nullptr},
    {0x20, 1, 1, KeyKind::Normal, nullptr}, {0x21, 1, 1, KeyKind::Normal, nullptr},
    {0x22, 1, 1, KeyKind::Normal, nullptr}, {0x23, 1, 1, KeyKind::Normal, nullptr},
    {0x24, 1, 1, KeyKind::Normal, nullptr}, {0x25, 1, 1, KeyKind::Normal, nullptr},
    {0x26, 1, 1, KeyKind::Normal, nullptr}, {0x27, 1, 1, KeyKind::Normal, nullptr},
    {HID_MINUS, 1, 1, KeyKind::Normal, nullptr}, {HID_EQUALS, 1, 1, KeyKind::Normal, nullptr},
    {HID_BACKSPACE, 4, 1, KeyKind::Normal, "Bksp"},
};

// Row 1: Tab, Q-P, [ ] \.
const KeyDef kRow1[] = {
    {HID_TAB, 2, 1, KeyKind::Normal, "Tab"},
    {0x14, 1, 1, KeyKind::Normal, nullptr}, {0x1A, 1, 1, KeyKind::Normal, nullptr},
    {0x08, 1, 1, KeyKind::Normal, nullptr}, {0x15, 1, 1, KeyKind::Normal, nullptr},
    {0x17, 1, 1, KeyKind::Normal, nullptr}, {0x1C, 1, 1, KeyKind::Normal, nullptr},
    {0x18, 1, 1, KeyKind::Normal, nullptr}, {0x0C, 1, 1, KeyKind::Normal, nullptr},
    {0x12, 1, 1, KeyKind::Normal, nullptr}, {0x13, 1, 1, KeyKind::Normal, nullptr},
    {HID_LBRACKET, 1, 1, KeyKind::Normal, nullptr}, {HID_RBRACKET, 1, 1, KeyKind::Normal, nullptr},
    {HID_BACKSLASH, 2, 1, KeyKind::Normal, nullptr},
};

// Row 2: Caps Lock, A-L, ; ', Enter -- ANSI/ISO position, end of the home
// row, one row tall (an earlier draft spanned it into row 3 for a tall
// ISO-style Enter; on the real panel that read as an oversized, out-of-
// place block, not as "an old keyboard's Enter key", so back to one row).
// Caps is 3 units, wider than Row 1's Tab (2 units) -- the physical-
// keyboard row stagger (each row's first letter sits a little further
// right than the row above, not stacked in a rigid vertical grid) comes
// entirely from these left-edge keys' widths increasing row to row, the
// same way it does on a real keyboard: no separate offset needed.
const KeyDef kRow2[] = {
    {0, 3, 1, KeyKind::CapsLock, "Caps"},
    {0x04, 1, 1, KeyKind::Normal, nullptr}, {0x16, 1, 1, KeyKind::Normal, nullptr},
    {0x07, 1, 1, KeyKind::Normal, nullptr}, {0x09, 1, 1, KeyKind::Normal, nullptr},
    {0x0A, 1, 1, KeyKind::Normal, nullptr}, {0x0B, 1, 1, KeyKind::Normal, nullptr},
    {0x0D, 1, 1, KeyKind::Normal, nullptr}, {0x0E, 1, 1, KeyKind::Normal, nullptr},
    {0x0F, 1, 1, KeyKind::Normal, nullptr},
    {HID_SEMICOLON, 1, 1, KeyKind::Normal, nullptr}, {HID_APOSTROPHE, 1, 1, KeyKind::Normal, nullptr},
    {HID_ENTER, 2, 1, KeyKind::Normal, "Enter"},
};

// Row 3: Shift (4 units -- continuing row 2's stagger), Z-M, , . /, Shift.
const KeyDef kRow3[] = {
    {0, 4, 1, KeyKind::Shift, "Shift"},
    {0x1D, 1, 1, KeyKind::Normal, nullptr}, {0x1B, 1, 1, KeyKind::Normal, nullptr},
    {0x06, 1, 1, KeyKind::Normal, nullptr}, {0x19, 1, 1, KeyKind::Normal, nullptr},
    {0x05, 1, 1, KeyKind::Normal, nullptr}, {0x11, 1, 1, KeyKind::Normal, nullptr},
    {0x10, 1, 1, KeyKind::Normal, nullptr},
    {HID_COMMA, 1, 1, KeyKind::Normal, nullptr}, {HID_PERIOD, 1, 1, KeyKind::Normal, nullptr},
    {HID_SLASH, 1, 1, KeyKind::Normal, nullptr},
    {0, 2, 1, KeyKind::Shift, "Shift"},
};

// Row 4: Ctrl, `, Space, and the full arrow cluster (Up/Down now fit).
const KeyDef kRow4[] = {
    {0, 2, 1, KeyKind::Ctrl, "Ctrl"},
    {HID_GRAVE, 1, 1, KeyKind::Normal, nullptr},
    {HID_SPACE, 9, 1, KeyKind::Normal, ""},
    {HID_LEFT, 1, 1, KeyKind::Normal, "<"},
    {HID_RIGHT, 1, 1, KeyKind::Normal, ">"},
    {HID_UP, 1, 1, KeyKind::Normal, "^"},
    {HID_DOWN, 1, 1, KeyKind::Normal, "v"},
};

struct Row {
  const KeyDef* keys;
  int count;
};

const Row kRows[] = {
    {kRow0, sizeof(kRow0) / sizeof(kRow0[0])},
    {kRow1, sizeof(kRow1) / sizeof(kRow1[0])},
    {kRow2, sizeof(kRow2) / sizeof(kRow2[0])},
    {kRow3, sizeof(kRow3) / sizeof(kRow3[0])},
    {kRow4, sizeof(kRow4) / sizeof(kRow4[0])},
};
constexpr int kRowCount = sizeof(kRows) / sizeof(kRows[0]);

GfxRenderer* g_renderer = nullptr;
int g_fontId = 0;
int g_x = 0, g_y = 0, g_w = 0, g_h = 0;
int g_unitPx = 0;
int g_rowH = 0;
OskKeyCallback g_onKey = nullptr;

bool g_shiftArmed = false;
bool g_ctrlArmed = false;
bool g_capsLockOn = false;

uint8_t currentModifiers() {
  uint8_t mods = 0;
  if (g_shiftArmed) mods |= OSK_MOD_SHIFT_LEFT;
  if (g_ctrlArmed) mods |= OSK_MOD_CTRL_LEFT;
  return mods;
}

}  // namespace

void oskInit(GfxRenderer& renderer, int labelFontId, int x, int y, int width, int height,
             OskKeyCallback onKey) {
  g_renderer = &renderer;
  g_fontId = labelFontId;
  g_x = x;
  g_y = y;
  g_w = width;
  g_h = height;
  g_unitPx = width / kUnitsPerRow;
  g_rowH = height / kRowCount;
  g_onKey = onKey;
  g_shiftArmed = false;
  g_ctrlArmed = false;
  g_capsLockOn = false;
}

char oskHidToChar(uint8_t hid, uint8_t modifiers) {
  const bool shift = (modifiers & (OSK_MOD_SHIFT_LEFT | 0x20)) != 0;  // left or right shift bit

  if (hid >= 0x04 && hid <= 0x1D) {
    const char base = 'a' + (hid - 0x04);
    return (shift ^ g_capsLockOn) ? (base - 32) : base;
  }
  if (hid >= 0x1E && hid <= 0x27) {
    static const char unshifted[] = "1234567890";
    static const char shifted[] = "!@#$%^&*()";
    const int idx = hid - 0x1E;
    return shift ? shifted[idx] : unshifted[idx];
  }
  switch (hid) {
    case HID_ENTER: return '\n';
    case HID_TAB: return '\t';
    case HID_SPACE: return ' ';
    case HID_MINUS: return shift ? '_' : '-';
    case HID_EQUALS: return shift ? '+' : '=';
    case HID_LBRACKET: return shift ? '{' : '[';
    case HID_RBRACKET: return shift ? '}' : ']';
    case HID_BACKSLASH: return shift ? '|' : '\\';
    case HID_SEMICOLON: return shift ? ':' : ';';
    case HID_APOSTROPHE: return shift ? '"' : '\'';
    case HID_GRAVE: return shift ? '~' : '`';
    case HID_COMMA: return shift ? '<' : ',';
    case HID_PERIOD: return shift ? '>' : '.';
    case HID_SLASH: return shift ? '?' : '/';
    default: return 0;
  }
}

bool oskShiftArmed() { return g_shiftArmed; }
bool oskCtrlArmed() { return g_ctrlArmed; }
bool oskCapsLockOn() { return g_capsLockOn; }

namespace {

// Renders a KeyDef's label into `out` (>= 6 bytes: the longest fixed label
// above, "Shift"/"Enter", plus the terminator). Labels are plain ASCII
// (0x20-0x7E) deliberately -- FONT_UI (NotoSans) was checked against its own
// interval table and does not cover the Unicode arrow/symbol glyphs an
// earlier draft of this file used (U+2190 etc all fell in gaps), which would
// have rendered as silently blank keys. Empty string for Space (drawn blank
// on purpose).
void keyLabel(const KeyDef& k, char* out, size_t outSize) {
  if (k.label != nullptr) {
    std::snprintf(out, outSize, "%s", k.label);
    return;
  }
  const char c = oskHidToChar(k.hid, currentModifiers());
  if (c >= 0x20 && c < 0x7F) {
    out[0] = c;
    out[1] = '\0';
  } else {
    out[0] = '\0';
  }
}

// A key's actual drawn/hit-tested rectangle, accounting for rowSpan.
struct KeyRect {
  int x, y, w, h;
};

KeyRect keyRect(const KeyDef& k, int rowIndex, int cx) {
  KeyRect out;
  out.x = cx;
  out.y = g_y + rowIndex * g_rowH;
  out.w = k.units * g_unitPx;
  out.h = k.rowSpan * g_rowH;
  return out;
}

bool rectContains(const KeyRect& r, int x, int y) {
  return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

}  // namespace

void oskDraw() {
  if (!g_renderer) return;

  for (int r = 0; r < kRowCount; r++) {
    const Row& row = kRows[r];
    int cx = g_x;
    for (int i = 0; i < row.count; i++) {
      const KeyDef& k = row.keys[i];
      const KeyRect rect = keyRect(k, r, cx);

      const bool armed = (k.kind == KeyKind::Shift && g_shiftArmed) ||
                          (k.kind == KeyKind::Ctrl && g_ctrlArmed) ||
                          (k.kind == KeyKind::CapsLock && g_capsLockOn);

      // Inset by 2px so adjacent keys show a visible gap instead of sharing
      // an edge -- makes individual keys legible as separate touch targets.
      const int inset = 2;
      if (armed) {
        g_renderer->fillRect(rect.x + inset, rect.y + inset, rect.w - 2 * inset,
                              rect.h - 2 * inset, true);
      } else {
        g_renderer->drawRect(rect.x + inset, rect.y + inset, rect.w - 2 * inset,
                              rect.h - 2 * inset, true);
      }

      char label[8];
      keyLabel(k, label, sizeof(label));
      if (label[0] != '\0') {
        const int tw = g_renderer->getTextWidth(g_fontId, label);
        const int tx = rect.x + (rect.w - tw) / 2;
        const int ty = rect.y + (rect.h - g_renderer->getLineHeight(g_fontId)) / 2;
        // armed keys are filled black; draw the label in white (state=false)
        // so it stays legible instead of vanishing into the fill.
        g_renderer->drawText(g_fontId, tx, ty, label, !armed);
      }

      cx += rect.w;
    }
  }
}

bool oskHandleTap(int logicalX, int logicalY) {
  if (logicalX < g_x || logicalX >= g_x + g_w || logicalY < g_y || logicalY >= g_y + g_h) {
    return false;
  }

  // Row-spanning keys (Enter) mean a tap can belong to a key declared in an
  // EARLIER row than the one its Y coordinate falls in, so every key's
  // actual rect is checked directly rather than pre-selecting one row by Y.
  for (int r = 0; r < kRowCount; r++) {
    const Row& row = kRows[r];
    int cx = g_x;
    for (int i = 0; i < row.count; i++) {
      const KeyDef& k = row.keys[i];
      const KeyRect rect = keyRect(k, r, cx);
      if (rectContains(rect, logicalX, logicalY)) {
        switch (k.kind) {
          case KeyKind::Shift:
            g_shiftArmed = !g_shiftArmed;
            return true;
          case KeyKind::Ctrl:
            g_ctrlArmed = !g_ctrlArmed;
            return true;
          case KeyKind::CapsLock:
            g_capsLockOn = !g_capsLockOn;
            return true;
          case KeyKind::Normal: {
            const uint8_t mods = currentModifiers();
            if (g_onKey) g_onKey(k.hid, mods);
            // One-shot: Shift/Ctrl apply to exactly the next normal key,
            // then clear themselves -- the standard touch-keyboard
            // "temporary shift" instead of a held key. Caps Lock is a
            // separate, genuinely sticky toggle and is untouched here.
            g_shiftArmed = false;
            g_ctrlArmed = false;
            return true;
          }
        }
      }
      cx += rect.w;
    }
  }
  return true;  // tap landed in the keyboard area but between/outside keys
}
