#pragma once

// ---------------------------------------------------------------------------
// dead_keys.h — US-International dead key composition for MicroSlate
//
// Implements the five dead keys of the US-International layout:
//   '  (apostrophe)  → acute accent   : a→á  e→é  i→í  o→ó  u→ú  c→ç
//   `  (grave)       → grave accent   : a→à  e→è  i→ì  o→ò  u→ù
//   ~  (tilde)       → tilde          : a→ã  o→õ  n→ñ
//   ^  (circumflex)  → circumflex     : a→â  e→ê  i→î  o→ô  u→û
//   "  (diaeresis)   → umlaut/trema   : a→ä  e→ë  i→ï  o→ö  u→ü
//
// Usage:
//   const char* result = deadKeyProcess(ch);
//   if (result == nullptr)  → insert `ch` normally (no dead key involved)
//   if (result[0] == '\0')  → dead key stored; insert nothing now
//   otherwise               → insert the UTF-8 string `result`
//
// When a dead key is followed by a non-composable character, the dead key
// is emitted as a literal via deadKeyFlush(), and the original character
// is returned via deadKeyPendingChar() for the caller to re-process.
// ---------------------------------------------------------------------------

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Composition table
// ---------------------------------------------------------------------------

typedef struct {
    char dead;
    char base;
    const char* result;  // UTF-8 encoded composed character
} DeadKeyEntry;

static const DeadKeyEntry DEAD_KEY_TABLE[] = {
    // --- Acute accent  ' ---
    { '\'', 'a', "\xC3\xA1" },  // á
    { '\'', 'A', "\xC3\x81" },  // Á
    { '\'', 'e', "\xC3\xA9" },  // é
    { '\'', 'E', "\xC3\x89" },  // É
    { '\'', 'i', "\xC3\xAD" },  // í
    { '\'', 'I', "\xC3\x8D" },  // Í
    { '\'', 'o', "\xC3\xB3" },  // ó
    { '\'', 'O', "\xC3\x93" },  // Ó
    { '\'', 'u', "\xC3\xBA" },  // ú
    { '\'', 'U', "\xC3\x9A" },  // Ú
    { '\'', 'c', "\xC3\xA7" },  // ç
    { '\'', 'C', "\xC3\x87" },  // Ç
    { '\'', ' ', "'"         },  // literal apostrophe

    // --- Grave accent  ` ---
    { '`',  'a', "\xC3\xA0" },  // à
    { '`',  'A', "\xC3\x80" },  // À
    { '`',  'e', "\xC3\xA8" },  // è
    { '`',  'E', "\xC3\x88" },  // È
    { '`',  'i', "\xC3\xAC" },  // ì
    { '`',  'I', "\xC3\x8C" },  // Ì
    { '`',  'o', "\xC3\xB2" },  // ò
    { '`',  'O', "\xC3\x92" },  // Ò
    { '`',  'u', "\xC3\xB9" },  // ù
    { '`',  'U', "\xC3\x99" },  // Ù
    { '`',  ' ', "`"         },  // literal grave

    // --- Tilde  ~ ---
    { '~',  'a', "\xC3\xA3" },  // ã
    { '~',  'A', "\xC3\x83" },  // Ã
    { '~',  'o', "\xC3\xB5" },  // õ
    { '~',  'O', "\xC3\x95" },  // Õ
    { '~',  'n', "\xC3\xB1" },  // ñ
    { '~',  'N', "\xC3\x91" },  // Ñ
    { '~',  ' ', "~"         },  // literal tilde

    // --- Circumflex  ^ ---
    { '^',  'a', "\xC3\xA2" },  // â
    { '^',  'A', "\xC3\x82" },  // Â
    { '^',  'e', "\xC3\xAA" },  // ê
    { '^',  'E', "\xC3\x8A" },  // Ê
    { '^',  'i', "\xC3\xAE" },  // î
    { '^',  'I', "\xC3\x8E" },  // Î
    { '^',  'o', "\xC3\xB4" },  // ô
    { '^',  'O', "\xC3\x94" },  // Ô
    { '^',  'u', "\xC3\xBB" },  // û
    { '^',  'U', "\xC3\x9B" },  // Û
    { '^',  ' ', "^"         },  // literal circumflex

    // --- Diaeresis / Trema  " ---
    { '"',  'a', "\xC3\xA4" },  // ä
    { '"',  'A', "\xC3\x84" },  // Ä
    { '"',  'e', "\xC3\xAB" },  // ë
    { '"',  'E', "\xC3\x8B" },  // Ë
    { '"',  'i', "\xC3\xAF" },  // ï
    { '"',  'I', "\xC3\x8F" },  // Ï
    { '"',  'o', "\xC3\xB6" },  // ö
    { '"',  'O', "\xC3\x96" },  // Ö
    { '"',  'u', "\xC3\xBC" },  // ü
    { '"',  'U', "\xC3\x9C" },  // Ü
    { '"',  ' ', "\""        },  // literal double-quote

    { 0, 0, 0 }  // sentinel
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static char  _dead_pending      = 0;   // the stored dead key, or 0
static char  _dead_requeue_char = 0;   // char to reprocess after flush

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Returns true if `ch` is one of the five dead keys.
static inline bool deadKeyIsDeadChar(char ch) {
    return ch == '\'' || ch == '`' || ch == '~' || ch == '^' || ch == '"';
}

// Returns true if there is a dead key waiting to be resolved.
static inline bool deadKeyHasPending() {
    return _dead_pending != 0;
}

// Returns the literal string for the pending dead key (so the caller can
// emit it as-is before emitting the non-composable character).
// Call only when deadKeyHasPending() is true.
static inline const char* deadKeyFlush() {
    static char buf[2];
    buf[0] = _dead_pending;
    buf[1] = '\0';
    _dead_pending = 0;
    return buf;
}

// After a flush, call this to retrieve the non-composable character that
// originally triggered the flush (0 if none).
static inline char deadKeyTakeRequeue() {
    char c = _dead_requeue_char;
    _dead_requeue_char = 0;
    return c;
}

// Main entry point.  Call with each printable ASCII character as it arrives.
//
// Returns:
//   nullptr  — no dead key involved; insert `ch` normally
//   ""       — dead key stored; do not insert anything yet
//   other    — UTF-8 string to insert (may be 1–3 bytes + NUL)
//
// When this returns a non-empty string AND deadKeyTakeRequeue() != 0,
// the caller must also re-process the requeued character.
static const char* deadKeyProcess(char ch) {
    if (_dead_pending != 0) {
        char dead = _dead_pending;
        _dead_pending = 0;

        // Search composition table
        for (int i = 0; DEAD_KEY_TABLE[i].result != 0; i++) {
            if (DEAD_KEY_TABLE[i].dead == dead && DEAD_KEY_TABLE[i].base == ch) {
                return DEAD_KEY_TABLE[i].result;  // composed character
            }
        }

        // No composition found: emit dead key as literal, requeue `ch`
        static char flush_buf[2];
        flush_buf[0] = dead;
        flush_buf[1] = '\0';
        _dead_requeue_char = ch;
        return flush_buf;
    }

    if (deadKeyIsDeadChar(ch)) {
        _dead_pending = ch;
        return "";  // stored; nothing to insert now
    }

    return nullptr;  // normal character, insert as-is
}

// Reset dead key state (e.g. on ESC, focus change, or backspace logic).
static inline void deadKeyReset() {
    _dead_pending      = 0;
    _dead_requeue_char = 0;
}

#ifdef __cplusplus
}
#endif
