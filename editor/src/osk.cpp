#include "osk.h"

#include <GfxRenderer.h>

#include <cstdio>
#include <cstring>

namespace {

enum class KeyKind : uint8_t { Normal, Shift, Ctrl, CapsLock, Alt };

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
constexpr uint8_t HID_ESCAPE = OSK_HID_ESCAPE;
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

// Row F: Esc alone, in its own row above everything else -- the way a real
// keyboard's function row sits above the main block, but without actually
// adding F1-F12 (which would clutter this small a panel for little benefit
// here). Same height as every other row (kRowCount grew to 6, so every
// row's share of the fixed OSK_H budget shrank a little to make room) --
// simpler than giving this one row a different height for one key. Nothing
// else lives on this row; the rest of its width is blank on purpose,
// matching how Esc sits alone in a physical keyboard's corner.
const KeyDef kRowEsc[] = {
    {HID_ESCAPE, 4, 1, KeyKind::Normal, "Esc"},
};

// Row 0: ` ~, digits, - =, Backspace. Esc used to open this row (see
// kRowEsc above for why it moved out); Backspace absorbs the 2 half-units
// that freed up, on top of its own earlier size -- both are the "growing
// Bksp" trade for Esc leaving.
const KeyDef kRow0[] = {
    {HID_GRAVE, 2, 1, KeyKind::Normal, nullptr},
    {0x1E, 2, 1, KeyKind::Normal, nullptr}, {0x1F, 2, 1, KeyKind::Normal, nullptr},
    {0x20, 2, 1, KeyKind::Normal, nullptr}, {0x21, 2, 1, KeyKind::Normal, nullptr},
    {0x22, 2, 1, KeyKind::Normal, nullptr}, {0x23, 2, 1, KeyKind::Normal, nullptr},
    {0x24, 2, 1, KeyKind::Normal, nullptr}, {0x25, 2, 1, KeyKind::Normal, nullptr},
    {0x26, 2, 1, KeyKind::Normal, nullptr}, {0x27, 2, 1, KeyKind::Normal, nullptr},
    {HID_MINUS, 2, 1, KeyKind::Normal, nullptr}, {HID_EQUALS, 2, 1, KeyKind::Normal, nullptr},
    {HID_BACKSPACE, 6, 1, KeyKind::Normal, "Bksp"},
};

// Row 1: Tab (4 half-units, back down from 5 -- the extra half-unit it had
// gained was specifically to stay aligned under Esc+`; now that Esc has
// its own row above, that reason is gone), Q-P, [ ] \ (back to a normal 2
// half-units, was shrunk to 3), then a small unlabeled "Enter" key filling
// the 2 half-units backslash gave up. That notch sits directly above the
// RIGHT part of row 2's (wider) Enter key below -- see kRow2's comment --
// so together the two pieces read as one L-shaped/ISO-style Enter reaching
// up into this row, without the earlier full-width two-row-tall Enter that
// looked oversized on the physical panel.
const KeyDef kRow1[] = {
    {HID_TAB, 4, 1, KeyKind::Normal, "Tab"},
    {0x14, 2, 1, KeyKind::Normal, nullptr}, {0x1A, 2, 1, KeyKind::Normal, nullptr},
    {0x08, 2, 1, KeyKind::Normal, nullptr}, {0x15, 2, 1, KeyKind::Normal, nullptr},
    {0x17, 2, 1, KeyKind::Normal, nullptr}, {0x1C, 2, 1, KeyKind::Normal, nullptr},
    {0x18, 2, 1, KeyKind::Normal, nullptr}, {0x0C, 2, 1, KeyKind::Normal, nullptr},
    {0x12, 2, 1, KeyKind::Normal, nullptr}, {0x13, 2, 1, KeyKind::Normal, nullptr},
    {HID_LBRACKET, 2, 1, KeyKind::Normal, nullptr}, {HID_RBRACKET, 2, 1, KeyKind::Normal, nullptr},
    {HID_BACKSLASH, 2, 1, KeyKind::Normal, nullptr},
    {HID_ENTER, 2, 1, KeyKind::Normal, nullptr},  // Enter's notch -- see comment above
};

// Row 2: Caps Lock (5 half-units, back down from 6, for the same reason
// Tab shrank -- see kRow1's comment), A-L, ; ', Enter (5 half-units, up
// from 4 -- the main, labeled body of the L-shaped Enter; its right 2
// half-units sit directly under row 1's unlabeled notch above).
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

// Row 3: Shift (7 half-units), Z-M, , . / (back to its normal 2 half-units),
// Up, Shift. Up and Right Shift moved one column left of an earlier draft
// (which had / shrunk to 1 half-unit and Up/RShift after it) -- / is full
// size again, Up sits right after it, and Right Shift shifts right of Up.
// Aligns 3-wide with row 4 below: / over Left, Up over Down, RShift over
// Right (all three pulled one half-unit left of the previous draft to make
// that 3-column match, not just Up-over-Down alone).
//
// Deliberately UNCHANGED by the row 0-2 rework above (Esc moving out,
// Tab/Caps shrinking, Enter growing): shrinking Left Shift to match would
// shift everything after it and break the / -> Left / Up -> Down / RShift
// -> Right alignment above without a specific replacement width to aim
// for, so this row (and row 4) is left exactly as it was rather than
// guessed at.
const KeyDef kRow3[] = {
    {0, 7, 1, KeyKind::Shift, "Shift"},
    {0x1D, 2, 1, KeyKind::Normal, nullptr}, {0x1B, 2, 1, KeyKind::Normal, nullptr},
    {0x06, 2, 1, KeyKind::Normal, nullptr}, {0x19, 2, 1, KeyKind::Normal, nullptr},
    {0x05, 2, 1, KeyKind::Normal, nullptr}, {0x11, 2, 1, KeyKind::Normal, nullptr},
    {0x10, 2, 1, KeyKind::Normal, nullptr},
    {HID_COMMA, 2, 1, KeyKind::Normal, nullptr}, {HID_PERIOD, 2, 1, KeyKind::Normal, nullptr},
    {HID_SLASH, 2, 1, KeyKind::Normal, nullptr},
    {HID_UP, 2, 1, KeyKind::Normal, "^"},
    {0, 2, 1, KeyKind::Shift, "Shift"},
};

// Row 4: Ctrl (5 half-units, was 4 -- joins the same cascade the other
// rows' left-edge keys got), Alt, Space, Left/Down/Right -- pulled one
// half-unit left of an earlier draft (Space 18->17) so the whole cluster
// lines up 3-wide with row 3's /, Up, Shift above it (see kRow3's comment).
// ` moved to row 0 (alongside Esc).
const KeyDef kRow4[] = {
    {0, 5, 1, KeyKind::Ctrl, "Ctrl"},
    {0, 3, 1, KeyKind::Alt, "Alt"},
    {HID_SPACE, 17, 1, KeyKind::Normal, ""},
    {HID_LEFT, 2, 1, KeyKind::Normal, "<"},
    {HID_DOWN, 2, 1, KeyKind::Normal, "v"},
    {HID_RIGHT, 2, 1, KeyKind::Normal, ">"},
};

struct Row {
  const KeyDef* keys;
  int count;
};

const Row kRows[] = {
    {kRowEsc, sizeof(kRowEsc) / sizeof(kRowEsc[0])},
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
bool g_altArmed = false;
bool g_capsLockOn = false;

uint8_t currentModifiers() {
  uint8_t mods = 0;
  if (g_shiftArmed) mods |= OSK_MOD_SHIFT_LEFT;
  if (g_ctrlArmed) mods |= OSK_MOD_CTRL_LEFT;
  if (g_altArmed) mods |= OSK_MOD_ALT_LEFT;
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
  g_altArmed = false;
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
                          (k.kind == KeyKind::Alt && g_altArmed) ||
                          (k.kind == KeyKind::CapsLock && g_capsLockOn);

      // Two-tier fill: functional keys (anything with a fixed multi-char
      // label -- Esc/Tab/Caps/Shift/Ctrl/Alt/Enter/Bksp/Space/arrows -- via
      // kind != Normal or a non-null label) get the darker of the two grays
      // GfxRenderer offers; plain letters/digits/symbols (kind == Normal,
      // label == nullptr) get the lighter one. Both are dither patterns
      // (DarkGray ~50% pixel density, LightGray ~25%) that work in plain BW
      // mode with no grayscale render pass -- not literal continuous gray
      // levels, so a dense checkerboard immediately behind small text can
      // read as visually busier than a real gray keycap would; worth a
      // second look on the physical panel specifically for that.
      const bool isFunctional = k.kind != KeyKind::Normal || k.label != nullptr;
      const Color fillColor = isFunctional ? Color::DarkGray : Color::LightGray;

      const int kx = rect.x + kInset, ky = rect.y + kInset;
      const int kw = rect.w - 2 * kInset, kh = rect.h - 2 * kInset;
      if (armed) {
        g_renderer->fillRoundedRect(kx, ky, kw, kh, kCornerRadius, Color::Black);
      } else {
        // Gray fill (see above) plus a black outline on top, rather than a
        // flat white key on a white background.
        g_renderer->fillRoundedRect(kx, ky, kw, kh, kCornerRadius, fillColor);
        g_renderer->drawRoundedRect(kx, ky, kw, kh, kBorderWidth, kCornerRadius, true);
      }

      // Whether a Shift-hint will be drawn in this key's top-right corner
      // (see below) -- decided first because, when there is one, the main
      // label below needs to make room for it. Only for keys where Shift
      // actually changes the character (digits/symbols) -- letters just
      // case-shift, which doesn't need this treatment any more than a real
      // keyboard dual-labels its letter keys.
      char hint[2] = {0, 0};
      if (k.label == nullptr && !(k.hid >= 0x04 && k.hid <= 0x1D)) {
        const char plain = oskHidToChar(k.hid, currentModifiers() & ~OSK_MOD_SHIFT_LEFT);
        const char shiftedCh = oskHidToChar(k.hid, currentModifiers() | OSK_MOD_SHIFT_LEFT);
        if (shiftedCh != 0 && shiftedCh != plain) hint[0] = shiftedCh;
      }
      const bool hasHint = hint[0] != '\0';

      // Main label: the key's current character (or fixed text for
      // Tab/Caps/Enter/etc), centered, using the larger label font. When a
      // Shift-hint is also being drawn, nudged down-left of true center --
      // matching a real keycap's off-center primary legend when a smaller
      // secondary symbol shares the top-right corner. Measured against
      // every digit/shift pair (1..0 vs !@#$%^&*()) in the two fonts this
      // actually uses: unshifted, the widest hint ('@', on the '2' key)
      // overlaps the centered main glyph's bounding box by 4x12px -- '%' on
      // '5' by 2x7px -- which is what read as the hint "touching" the main
      // character. A 4px-left/4px-down nudge clears every pair with margin
      // to spare; smaller nudges still leave '2'/'@' overlapping.
      constexpr int kHintClearDx = -4;
      constexpr int kHintClearDy = 4;
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
        int tx = rect.x + (rect.w - tw) / 2;
        int ty = rect.y + (rect.h - g_renderer->getLineHeight(g_fontId)) / 2;
        if (hasHint) {
          tx += kHintClearDx;
          ty += kHintClearDy;
        }
        // armed keys are filled black; draw the label in white (state=false)
        // so it stays legible instead of vanishing into the fill.
        g_renderer->drawText(g_fontId, tx, ty, label, !armed);
      }

      // Small Shift-hint in the top-RIGHT corner: what this key produces
      // WITH Shift, shown even while Shift isn't currently armed -- matches
      // a real keycap's dual legend (small shifted symbol above the main
      // character), so someone who doesn't remember e.g. Shift+7 = & can
      // still see it's coming rather than being surprised only after
      // pressing Shift.
      if (hasHint) {
        // A few px clear of both the top and right edges rather than
        // flush against them -- flush read as glued to the border on the
        // physical panel.
        constexpr int kHintMargin = 5;
        const int hintW = g_renderer->getTextWidth(g_smallFontId, hint);
        g_renderer->drawText(g_smallFontId, rect.x + rect.w - kInset - kHintMargin - hintW,
                              rect.y + kInset + kHintMargin, hint, !armed);
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
          case KeyKind::Alt:
            g_altArmed = !g_altArmed;
            return true;
          case KeyKind::CapsLock:
            g_capsLockOn = !g_capsLockOn;
            return true;
          case KeyKind::Normal: {
            const uint8_t mods = currentModifiers();
            if (g_onKey) g_onKey(k.hid, mods);
            // One-shot: Shift/Ctrl/Alt apply to exactly the next normal key,
            // then clear themselves -- the standard touch-keyboard
            // "temporary shift" instead of a held key. Caps Lock is a
            // separate, genuinely sticky toggle and is untouched here.
            g_shiftArmed = false;
            g_ctrlArmed = false;
            g_altArmed = false;
            return true;
          }
        }
      }
      cx += rect.w;
    }
  }
  return true;  // tap landed in the keyboard area but between/outside keys
}
