#pragma once

#include <cstdint>

#include "config.h"

void editorInit();
void editorClear();
void editorLoadBuffer(size_t length);  // After filling buffer externally, set length + reset cursor

// Buffer access
char* editorGetBuffer();
size_t editorGetLength();
int editorGetCursorPosition();

// Editing operations
void editorInsertChar(char c);
void editorInsertUtf8(const char* utf8str);  // Insert a UTF-8 string byte by byte
void editorDeleteChar();     // Backspace
void editorDeleteForward();  // Delete key

// Cursor movement. `extendSelection` (Shift held) grows/keeps a selection
// from wherever the cursor was before this call; a plain move clears it.
void editorMoveCursorLeft(bool extendSelection = false);
void editorMoveCursorRight(bool extendSelection = false);
void editorMoveCursorUp(bool extendSelection = false);
void editorMoveCursorDown(bool extendSelection = false);
void editorMoveCursorHome(bool extendSelection = false);
void editorMoveCursorEnd(bool extendSelection = false);

// Selection: editorMoveCursor*(true) sets the anchor on first extend;
// cursorPosition is always the live end. A plain move, insert, or a fresh
// buffer (editorInit/editorClear/editorLoadBuffer) clears it — there's no
// "collapse to an edge" step, movement without Shift always just moves.
void editorClearSelection();
void editorSelectAll();         // no-op on an empty buffer
bool editorHasSelection();      // false when the range is empty (anchor == cursor)
int editorGetSelectionStart();  // normalized (<= end); -1 if none
int editorGetSelectionEnd();    // normalized; -1 if none
void editorDeleteSelection();   // no-op if none

// Single-slot clipboard (there's no OS clipboard on this device). Persists
// across documents — copy in one note, paste in another — until overwritten
// by the next copy/cut.
void editorCopySelection();   // no-op if no selection
void editorCutSelection();    // copy, then editorDeleteSelection()
void editorPasteAtCursor();   // no-op if the clipboard is empty
void editorPasteOverSelection();  // substitui a selecao como UMA operacao
bool editorHasClipboardContent();

// --- Desfazer, um nivel ---------------------------------------------------
// Guarda a ULTIMA operacao em bloco -- apagar selecao, cortar, colar -- e a
// desfaz. Existe pelo caso que se perde de verdade: selecionar um trecho e
// apaga-lo com DEL ou BACKSPACE, que ao contrario do cortar nao deixa copia
// no clipboard.
//
// Deliberadamente NAO cobre digitacao. Um registro por caractere digitado
// custaria o mesmo que uma pilha de desfazer inteira, e um registro que
// sobrevive a digitacao seria pior: apertar Ctrl+Z depois de escrever um
// paragrafo reinseriria um trecho apagado minutos antes, do nada. Entao
// qualquer edicao que nao seja bloco **descarta** o registro, e o Ctrl+Z
// passa a nao fazer nada -- previsivel, mesmo que limitado.
void editorUndo();
bool editorCanUndo();

// Line/viewport management
//
// Word-wrap measures real glyph widths (see editorSetGlyphWidthFn) against
// a pixel budget rather than an estimated character count, so it can never
// produce a line wider than that budget regardless of font or character mix.
void editorSetMaxLineWidthPx(int px);
// Per-codepoint pixel width lookup for the wrap pass. This module has no
// font/renderer dependency of its own — the caller (which does) supplies
// this once at startup. Passing nullptr falls back to a fixed guess.
void editorSetGlyphWidthFn(int (*fn)(uint32_t codepoint));
void editorSetVisibleLines(int n);   // Tell editor how many lines are visible on screen
int editorGetStoredVisibleLines();   // Get the last set visible lines count

// PgUp/PgDn jump size, in lines. Deliberately separate from
// editorSetVisibleLines(): Typewriter mode forces that to 1 (only one line
// is ever drawn), which would make a page jump indistinguishable from a
// single arrow press — exactly the size PgUp/PgDn needs to be usable on a
// long note. The renderer sets this once per frame from screen geometry,
// independent of which writing mode is actually active.
void editorSetPageJumpLines(int n);
int editorGetPageJumpLines();
void editorRecalculateLines();
int editorGetVisibleLines(int lineHeight, int textAreaHeight);
int editorGetViewportStart();
int editorGetCursorLine();
int editorGetCursorCol();
int editorGetLineCount();
int editorGetLinePosition(int lineIndex);

// File metadata
void editorSetCurrentFile(const char* filename);
void editorSetCurrentTitle(const char* title);
const char* editorGetCurrentFile();
const char* editorGetCurrentTitle();
bool editorHasUnsavedChanges();
void editorSetUnsavedChanges(bool v);
int editorGetWordCount();
