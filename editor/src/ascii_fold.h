#pragma once

#include <cstdint>

// Folds an accented character down to the closest plain ASCII letter, for
// building SD card filenames.
//
// This is not a style preference. SdFat refuses any byte with the high bit
// set in a long file name:
//
//     inline bool lfnLegalChar(uint8_t c) {
//       return !(lfnReservedChar(c) || c & 0X80);
//     }
//
// so `SAVE "ação"` failed with "File Error" -- the open, not the writing.
// SdFat can be built with USE_UTF8_LONG_NAMES, but its own config warns that
// it costs "significantly more flash memory and a small amount of extra RAM",
// and RAM is the resource this firmware has spent the most effort clawing
// back (see docs/DEVELOPMENT_LOG.md). It would also change how every file on
// a card shared with the reader firmwares is named.
//
// Folding instead keeps names typeable, and keeps them *findable*: SAVE and
// LOAD both apply the same rule, so `LOAD "ação"` opens the file `SAVE
// "ação"` created, and `LOAD "acao"` opens it too.
//
// Takes a codepoint rather than a byte because the two callers hold different
// encodings -- BASIC strings are Latin-1 (one byte per character, so the byte
// is the codepoint), while the editors' titles are UTF-8 and must be decoded
// first. Returns 0 for anything with no sensible ASCII stand-in, which the
// caller drops or replaces as it sees fit.
inline char asciiFold(uint32_t cp) {
  if (cp < 0x80) return (char)cp;
  if (cp > 0xFF) return 0;

  // Latin-1 supplement. Uppercase folds to lowercase directly, since every
  // caller lower-cases anyway.
  switch (cp) {
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: case 0xC6:
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6:
      return 'a';
    case 0xC7: case 0xE7: return 'c';
    case 0xC8: case 0xC9: case 0xCA: case 0xCB:
    case 0xE8: case 0xE9: case 0xEA: case 0xEB:
      return 'e';
    case 0xCC: case 0xCD: case 0xCE: case 0xCF:
    case 0xEC: case 0xED: case 0xEE: case 0xEF:
      return 'i';
    case 0xD0: case 0xF0: return 'd';
    case 0xD1: case 0xF1: return 'n';
    case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8:
    case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF8:
      return 'o';
    case 0xD9: case 0xDA: case 0xDB: case 0xDC:
    case 0xF9: case 0xFA: case 0xFB: case 0xFC:
      return 'u';
    case 0xDD: case 0xFD: case 0xFF: return 'y';
    case 0xDF: return 's';  // sharp s, folded to one letter to keep names short
    default: return 0;      // math signs, thorn, punctuation: no stand-in
  }
}
