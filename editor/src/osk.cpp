#include "osk.h"

#include <GfxRenderer.h>

#include <cstdio>
#include <cstring>

namespace {

enum class KeyKind : uint8_t { Normal, Shift, Ctrl, CapsLock };

struct KeyDef {
  uint8_t hid;      // USB HID usage ID; 0 for modifier-only keys (kind != Normal)
  uint8_t units;    // width in half-units (see kUnitsPerRow below)
  uint8_t rowSpan;  // height in rows; currently always 1 (kept general, see below)
  KeyKind kind;
  const char* label;  // fixed label; nullptr means "derive from hid via oskHidToChar()"
};

// USB HID keyboard usage IDs this layout needs. Letters (0x04-0x1D, A-Z
// alphabetical) and digits (0x1E-0x27, 1-9 then 0) are generated in the row
// tables below rather than named individually.
constexpr uint8_t HID_ESCAPE = 0x29;
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

// 32 HALF-units/row (960/32 = 30px/half-unit -- a normal 1-key-wide key is 2
// half-units = 60px). The grid is in half-units, not whole units, so the row
// stagger below can step by half a key at a time: on a real keyboard Tab/
// Caps/Shift each get a little wider than the last, by roughly half a key,
// not a whole one -- a whole-key step (an earlier draft) read as too coarse
// on the physical panel.
constexpr int kUnitsPerRow = 32;

// Row 0: Esc (there is no separate function-key row on this 5-row layout,
// so it takes the top-left slot the way compact/tenkeyless boards do --
// functionally important, not just conventional: input_handler.cpp treats
// Escape and Ctrl+C as BASIC's two "stop the program" gestures), digits,
// - =, Backspace.
const KeyDef kRow0[] = {
    {HID_ESCAPE, 2, 1, KeyKind::Normal, "Esc"},
    {0x1E, 2, 1, KeyKind::Normal, nullptr}, {0x1F, 2, 1, KeyKind::Normal, nullptr},
    {0x20, 2, 1, KeyKind::Normal, nullptr}, {0x21, 2, 1, KeyKind::Normal, nullptr},
    {0x22, 2, 1, KeyKind::Normal, nullptr}, {0x23, 2, 1, KeyKind::Normal, nullptr},
    {0x24, 2, 1, KeyKind::Normal, nullptr}, {0x25, 2, 1, KeyKind::Normal, nullptr},
    {0x26, 2, 1, KeyKind::Normal, nullptr}, {0x27, 2, 1, KeyKind::Normal, nullptr},
    {HID_MINUS, 2, 1, KeyKind::Normal, nullptr}, {HID_EQUALS, 2, 1, KeyKind::Normal, nullptr},
    {HID_BACKSPACE, 6, 1, KeyKind::Normal, "Bksp"},
};

// Row 1: Tab, Q-P, [ ] \.
const KeyDef kRow1[] = {
    {HID_TAB, 4, 1, KeyKind::Normal, "Tab"},
    {0x14, 2, 1, KeyKind::Normal, nullptr}, {0x1A, 2, 1, KeyKind::Normal, nullptr},
    {0x08, 2, 1, KeyKind::Normal, nullptr}, {0x15, 2, 1, KeyKind::Normal, nullptr},
    {0x17, 2, 1, KeyKind::Normal, nullptr}, {0x1C, 2, 1, KeyKind::Normal, nullptr},
    {0x18, 2, 1, KeyKind::Normal, nullptr}, {0x0C, 2, 1, KeyKind::Normal, nullptr},
    {0x12, 2, 1, KeyKind::Normal, nullptr}, {0x13, 2, 1, KeyKind::Normal, nullptr},
    {HID_LBRACKET, 2, 1, KeyKind::Normal, nullptr}, {HID_RBRACKET, 2, 1, KeyKind::Normal, nullptr},
    {HID_BACKSLASH, 4, 1, KeyKind::Normal, nullptr},
};

// Row 2: Caps Lock (5 half-units -- half a key wider than Tab's 4), A-L, ;
// ', Enter (ANSI end-of-home-row position, one row tall).
const KeyDef kRow2[] = {
    {0, 5, 1, KeyKind::CapsLock, "Caps"},
    {0x04, 2, 1, KeyKind::Normal, nullptr}, {0x16, 2, 1, KeyKind::Normal, nullptr},
    {0x07, 2, 1, KeyKind::Normal, nullptr}, {0x09, 2, 1, KeyKind::Normal, nullptr},
    {0x0A, 2, 1, KeyKind::Normal, nullptr}, {0x0B, 2, 1, KeyKind::Normal, nullptr},
    {0x0D, 2, 1, KeyKind::Normal, nullptr}, {0x0E, 2, 1, KeyKind::Normal, nullptr},
    {0x0F, 2, 1, KeyKind::Normal, nullptr},
    {HID_SEMICOLON, 2, 1, KeyKind::Normal, nullptr}, {HID_APOSTROPHE, 2, 1, KeyKind::Normal, nullptr},
    {HID_ENTER, 5, 1, KeyKind::Normal, "Enter"},
};

// Row 3: Shift (6 half-units -- half a key wider than Caps's 5), Z-M, , . /,
// Shift.
const KeyDef kRow3[] = {
    {0, 6, 1, KeyKind::Shift, "Shift"},
    {0x1D, 2, 1, KeyKind::Normal, nullptr}, {0x1B, 2, 1, KeyKind::Normal, nullptr},
    {0x06, 2, 1, KeyKind::Normal, nullptr}, {0x19, 2, 1, KeyKind::Normal, nullptr},
    {0x05, 2, 1, KeyKind::Normal, nullptr}, {0x11, 2, 1, KeyKind::Normal, nullptr},
    {0x10, 2, 1, KeyKind::Normal, nullptr},
    {HID_COMMA, 2, 1, KeyKind::Normal, nullptr}, {HID_PERIOD, 2, 1, KeyKind::Normal, nullptr},
    {HID_SLASH, 2, 1, KeyKind::Normal, nullptr},
    {0, 6, 1, KeyKind::Shift, "Shift"},
};

// Row 4: Ctrl, `, Space, full arrow cluster.
const KeyDef kRow4[] = {
    {0, 4, 1, KeyKind::Ctrl, "Ctrl"},
    {HID_GRAVE, 2, 1, KeyKind::Normal, nullptr},
    {HID_SPACE, 18, 1, KeyKind::Normal, ""},
    {HID_LEFT, 2, 1, KeyKind::Normal, "<"},
    {HID_RIGHT, 2, 1, KeyKind::Normal, ">"},
    {HID_UP, 2, 1, KeyKind::Normal, "^"},
    {HID_DOWN, 2, 1, KeyKind::Normal, "v"},
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
int g_smallFontId = 0;
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

void oskInit(GfxRenderer& renderer, int labelFontId, int smallLabelFontId, int x, int y, int width,
             int height, OskKeyCallback onKey) {
  g_renderer = &renderer;
  g_fontId = labelFontId;
  g_smallFontId = smallLabelFontId;
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

// A key's actual drawn/hit-tested rectangle.
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

  // Corner radius and inter-key gap: bumped up from an earlier flush/sharp-
  // corner draft after real-hardware feedback that this panel's touch
  // reading is noticeably less precise than a phone's, so keys read more
  // clearly as separate targets with a bit more breathing room between
  // them, and rounded corners read less like a rigid uniform grid.
  constexpr int kInset = 4;
  constexpr int kCornerRadius = 6;
  constexpr int kBorderWidth = 2;

  for (int r = 0; r < kRowCount; r++) {
    const Row& row = kRows[r];
    int cx = g_x;
    for (int i = 0; i < row.count; i++) {
      const KeyDef& k = row.keys[i];
      const KeyRect rect = keyRect(k, r, cx);

      const bool armed = (k.kind == KeyKind::Shift && g_shiftArmed) ||
                          (k.kind == KeyKind::Ctrl && g_ctrlArmed) ||
                          (k.kind == KeyKind::CapsLock && g_capsLockOn);

      const int kx = rect.x + kInset, ky = rect.y + kInset;
      const int kw = rect.w - 2 * kInset, kh = rect.h - 2 * kInset;
      if (armed) {
        g_renderer->fillRoundedRect(kx, ky, kw, kh, kCornerRadius, Color::Black);
      } else {
        // Light-gray fill (a sparse 1-in-4-pixel dither -- GfxRenderer's
        // Color::LightGray works this way even in plain BW mode, no
        // grayscale render pass needed) plus a black outline on top, rather
        // than a flat white key on a white background.
        g_renderer->fillRoundedRect(kx, ky, kw, kh, kCornerRadius, Color::LightGray);
        g_renderer->drawRoundedRect(kx, ky, kw, kh, kBorderWidth, kCornerRadius, true);
      }

      // Main label: the key's current character (or fixed text for
      // Tab/Caps/Enter/etc), centered, using the larger label font.
      char label[8];
      if (k.label != nullptr) {
        std::snprintf(label, sizeof(label), "%s", k.label);
      } else {
        const char c = oskHidToChar(k.hid, currentModifiers());
        if (c >= 0x20 && c < 0x7F) {
          label[0] = c;
          label[1] = '\0';
        } else {
          label[0] = '\0';
        }
      }
      if (label[0] != '\0') {
        const int tw = g_renderer->getTextWidth(g_fontId, label);
        const int tx = rect.x + (rect.w - tw) / 2;
        const int ty = rect.y + (rect.h - g_renderer->getLineHeight(g_fontId)) / 2;
        // armed keys are filled black; draw the label in white (state=false)
        // so it stays legible instead of vanishing into the fill.
        g_renderer->drawText(g_fontId, tx, ty, label, !armed);
      }

      // Small Shift-hint in the top-left corner: what this key produces
      // WITH Shift, shown even while Shift isn't currently armed -- matches
      // a real keycap's dual legend (small shifted symbol above the main
      // character), so someone who doesn't remember e.g. Shift+7 = & can
      // still see it's coming rather than being surprised only after
      // pressing Shift. Only for keys where Shift actually changes the
      // character (digits/symbols) -- letters just case-shift, which
      // doesn't need this treatment any more than a real keyboard dual-
      // labels its letter keys.
      if (k.label == nullptr && !(k.hid >= 0x04 && k.hid <= 0x1D)) {
        const char plain = oskHidToChar(k.hid, currentModifiers() & ~OSK_MOD_SHIFT_LEFT);
        const char shifted = oskHidToChar(k.hid, currentModifiers() | OSK_MOD_SHIFT_LEFT);
        if (shifted != 0 && shifted != plain) {
          char hint[2] = {shifted, '\0'};
          g_renderer->drawText(g_smallFontId, rect.x + kInset + 2, rect.y + kInset, hint, !armed);
        }
      }

      cx += rect.w;
    }
  }
}

bool oskHandleTap(int logicalX, int logicalY) {
  if (logicalX < g_x || logicalX >= g_x + g_w || logicalY < g_y || logicalY >= g_y + g_h) {
    return false;
  }

  // Row-spanning keys would mean a tap can belong to a key declared in an
  // earlier row than the one its Y falls in, so every key's actual rect is
  // checked directly rather than pre-selecting one row by Y -- harmless
  // now that nothing spans more than one row, and correct if that changes.
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
