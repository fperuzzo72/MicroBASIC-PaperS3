#pragma once

// ---------------------------------------------------------------------------
// altgr_keys.h - the AltGr layer of the US-International layout
//
// GENERATED FILE - regenerate with tools/gen_microslate_patch.py in
// https://github.com/fperuzzo72/US-International-IME-Android
//
// Source of truth: a disassembly of the KBDUSX.DLL that ships with Windows,
// published at https://kbdlayout.info/kbdusx/
//
// This is the half of the layout the dead key engine does not cover. AltGr is
// the right Alt key (HID modifier bit 0x40, MOD_ALT_RIGHT in config.h).
//
// Usage, alongside deadKeyProcess():
//
//   if (isAltGr(modifiers)) {
//     const char* s = altGrToUtf8(keyCode, modifiers, capsLockOn);
//     if (s) {
//       // an AltGr character is a literal, so it resolves any pending
//       // dead key first, exactly like a non-composable letter would
//       if (deadKeyHasPending()) editorInsertUtf8(deadKeyFlush());
//       editorInsertUtf8(s);
//     }
//     return;  // AltGr positions the layout leaves empty type nothing
//   }
// ---------------------------------------------------------------------------

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>  // NULL

#include "config.h"  // MOD_ALT_RIGHT, isShift()

#ifdef __cplusplus
extern "C" {
#endif

// AltGr is the right Alt key. Left Alt stays a plain modifier, so Alt
// shortcuts keep working.
static inline bool isAltGr(uint8_t mod) {
    return (mod & MOD_ALT_RIGHT) != 0;
}

typedef struct {
    uint8_t     hid;      // USB HID usage id, same ids hidToAscii() uses
    const char* plain;    // AltGr
    const char* shifted;  // AltGr + Shift, or NULL where the layout is empty
    bool        caps;     // CapsLock swaps the two (real case pairs only)
} AltGrEntry;

static const AltGrEntry ALTGR_TABLE[] = {
    { 0x04, "\xC3\xA1",          "\xC3\x81",          true   },  // a -> á / shift Á
    { 0x06, "\xC2\xA9",          "\xC2\xA2",          false  },  // c -> © / shift ¢
    { 0x07, "\xC3\xB0",          "\xC3\x90",          true   },  // d -> ð / shift Ð
    { 0x08, "\xC3\xA9",          "\xC3\x89",          true   },  // e -> é / shift É
    { 0x0C, "\xC3\xAD",          "\xC3\x8D",          true   },  // i -> í / shift Í
    { 0x0F, "\xC3\xB8",          "\xC3\x98",          true   },  // l -> ø / shift Ø
    { 0x10, "\xC2\xB5",          NULL,                false  },  // m -> µ
    { 0x11, "\xC3\xB1",          "\xC3\x91",          true   },  // n -> ñ / shift Ñ
    { 0x12, "\xC3\xB3",          "\xC3\x93",          true   },  // o -> ó / shift Ó
    { 0x13, "\xC3\xB6",          "\xC3\x96",          true   },  // p -> ö / shift Ö
    { 0x14, "\xC3\xA4",          "\xC3\x84",          true   },  // q -> ä / shift Ä
    { 0x15, "\xC2\xAE",          NULL,                false  },  // r -> ®
    { 0x16, "\xC3\x9F",          "\xC2\xA7",          false  },  // s -> ß / shift §
    { 0x17, "\xC3\xBE",          "\xC3\x9E",          true   },  // t -> þ / shift Þ
    { 0x18, "\xC3\xBA",          "\xC3\x9A",          true   },  // u -> ú / shift Ú
    { 0x1A, "\xC3\xA5",          "\xC3\x85",          true   },  // w -> å / shift Å
    { 0x1C, "\xC3\xBC",          "\xC3\x9C",          true   },  // y -> ü / shift Ü
    { 0x1D, "\xC3\xA6",          "\xC3\x86",          true   },  // z -> æ / shift Æ
    { 0x1E, "\xC2\xA1",          "\xC2\xB9",          false  },  // 1 -> ¡ / shift ¹
    { 0x1F, "\xC2\xB2",          NULL,                false  },  // 2 -> ²
    { 0x20, "\xC2\xB3",          NULL,                false  },  // 3 -> ³
    { 0x21, "\xC2\xA4",          "\xC2\xA3",          false  },  // 4 -> ¤ / shift £
    { 0x22, "\xE2\x82\xAC",      NULL,                false  },  // 5 -> €
    { 0x23, "\xC2\xBC",          NULL,                false  },  // 6 -> ¼
    { 0x24, "\xC2\xBD",          NULL,                false  },  // 7 -> ½
    { 0x25, "\xC2\xBE",          NULL,                false  },  // 8 -> ¾
    { 0x26, "\xE2\x80\x98",      NULL,                false  },  // 9 -> ‘
    { 0x27, "\xE2\x80\x99",      NULL,                false  },  // 0 -> ’
    { 0x2D, "\xC2\xA5",          NULL,                false  },  // - -> ¥
    { 0x2E, "\xC3\x97",          "\xC3\xB7",          false  },  // = -> × / shift ÷
    { 0x2F, "\xC2\xAB",          NULL,                false  },  // [ -> «
    { 0x30, "\xC2\xBB",          NULL,                false  },  // ] -> »
    { 0x31, "\xC2\xAC",          "\xC2\xA6",          false  },  // \ -> ¬ / shift ¦
    { 0x33, "\xC2\xB6",          "\xC2\xB0",          false  },  // ; -> ¶ / shift °
    { 0x34, "\xC2\xB4",          "\xC2\xA8",          false  },  // ' -> ´ / shift ¨
    { 0x36, "\xC3\xA7",          "\xC3\x87",          true   },  // , -> ç / shift Ç
    { 0x38, "\xC2\xBF",          NULL,                false  },  // / -> ¿
    { 0x00, NULL, NULL, false }  // sentinel
};

// Returns the UTF-8 string for an AltGr combination, or NULL when the layout
// leaves that position empty (AltGr+F, for instance) and nothing should be
// typed at all.
static const char* altGrToUtf8(uint8_t hid, uint8_t modifiers, bool capsLockOn) {
    for (int i = 0; ALTGR_TABLE[i].hid != 0x00; i++) {
        if (ALTGR_TABLE[i].hid != hid) continue;
        bool shifted = isShift(modifiers);
        if (capsLockOn && ALTGR_TABLE[i].caps) shifted = !shifted;
        const char* out = shifted ? ALTGR_TABLE[i].shifted : ALTGR_TABLE[i].plain;
        if (out == NULL) out = ALTGR_TABLE[i].plain;  // fall back to the base level
        return out;
    }
    return NULL;
}

#ifdef __cplusplus
}
#endif
