#pragma once

// Single point of contact with the TinyBasic interpreter's headers
// (editor/lib/TinyBasic, fetched by patches/tinybasic/fetch.sh).
//
// This exists because getting the include *order* wrong is silently
// catastrophic rather than a compile error. `basic.h` picks the numeric type
// from feature macros:
//
//     #ifdef HASFLOAT
//     typedef float number_t;
//     #else
//     typedef int   number_t;
//
// and `HASFLOAT` comes from `language.h`. A translation unit that includes
// `basic.h` *without* `language.h` therefore gets `number_t = int` while
// `basic.c` itself has `number_t = float` -- the same global read as two
// different types. It compiles and links cleanly, then produces nonsense at
// runtime: reading the interpreter's `x` (a float 10.0, bits 0x41200000) as
// an int yields 1092616192, which truncated to a uint16 line number is 0.
//
// That is exactly the bug this file was written to prevent: every program
// line typed on the device was stored under line number 0, so LIST showed
// them all starting with "0" and RUN failed with "Unknown Line Error"
// because GOTO 10 had nothing to find. See docs/DEVELOPMENT_LOG.md.
//
// The order below matches basic.c's own includes exactly. Do not reorder,
// and do not include the interpreter's headers directly from anywhere else.

extern "C" {
#include "common.h"
#include "hardware.h"
#include "runtime.h"
#include "language.h"
#include "basic.h"

// The interpreter's own state. These are plain globals in basic.c and are
// not declared in any of its headers, so they are declared here once, with
// the types they are defined with there (checked against basic.c):
//
//   char ibuffer[BUFSIZE];  char *bi;  number_t x;  address_t ax;
//   token_t token;  token_t er;  mem_t st;  address_t here;  mem_t form;
//
extern char ibuffer[];
extern char* bi;
extern number_t x;
extern address_t ax;
extern token_t token;
extern token_t er;
extern mem_t st;
extern address_t here;
extern mem_t form;
extern address_t top;

// Entry point, renamed from setup() by patches/tinybasic/02 so it doesn't
// collide with the firmware's own.
void basicSetup();
}
