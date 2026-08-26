#include "text_editor.h"
#include <cstring>
#include <algorithm>
#include "Utf8.h"

// --- Text buffer ---
static char textBuffer[TEXT_BUFFER_SIZE];
static size_t textLength = 0;
static int cursorPosition = 0;

// --- File metadata ---
static char currentFile[MAX_FILENAME_LEN] = "";
static char currentTitle[MAX_TITLE_LEN] = "Untitled";
static bool unsavedChanges = false;

// --- Line management ---
static int linePositions[MAX_LINES];  // Index into textBuffer for start of each line
static int lineCount = 0;
static int cursorLine = 0;
static int cursorCol = 0;
static int viewportStartLine = 0;
static int maxLineWidthPx = 400;      // Wrap budget in pixels — see editorSetMaxLineWidthPx.
static int (*glyphWidthFn)(uint32_t) = nullptr;  // Per-codepoint pixel width; caller-supplied (font is a renderer concern, not this module's).
static int storedVisibleLines = 20;  // Updated by renderer each frame
static int pageJumpLines = 20;       // PgUp/PgDn jump size — see editorSetPageJumpLines
static bool lineBreaksDirty = true;  // Only recompute line breaks when buffer/maxLineWidthPx changes

// --- Selection & clipboard ---
static int selectionAnchor = -1;      // -1 = no selection; else the fixed end, cursorPosition is the live end
// Allocated on the first copy rather than reserved statically. 16KB of .bss
// sits below the heap, so as a static array it was 16KB permanently removed
// from the largest contiguous block -- on a device where the BLE connect task
// needs 20KB in one piece and has already failed to get it (see
// docs/DEVELOPMENT_LOG.md). A copy that cannot allocate simply does not copy,
// which is a far better failure than a keyboard that will not reconnect.
static char* clipboard = nullptr;

// --- Registro de desfazer (ver editorUndo em text_editor.h) ---------------
//
// Uma operacao em bloco e descrita por: na posicao `pos`, foram inseridos
// `undoInserted` bytes e removidos os `undoRemovedLen` bytes guardados em
// `undoRemoved`. Desfazer e o inverso, na ordem inversa: tira o que entrou,
// devolve o que saiu.
//
// Isso cobre os tres casos com uma estrutura so: apagar (inserted=0), colar
// (removed vazio) e colar por cima de uma selecao (os dois), que sem isto
// desfaria so metade da operacao.
//
// Alocado sob demanda como o clipboard, e pelo mesmo motivo: 16KB estaticos
// sairiam do maior bloco contiguo, que a task de conexao BLE precisa inteiro.
static char* undoRemoved = nullptr;
static size_t undoRemovedLen = 0;
static size_t undoInserted = 0;
static int undoPos = -1;

static void undoClear() { undoPos = -1; undoRemovedLen = 0; undoInserted = 0; }

// Chamada ANTES da operacao alterar o buffer.
static void undoRecord(int pos, const char* removed, size_t removedLen, size_t inserted) {
  undoClear();
  if (removedLen > 0) {
    if (!undoRemoved) {
      undoRemoved = (char*)malloc(TEXT_BUFFER_SIZE);
      if (!undoRemoved) return;  // sem memoria: fica sem desfazer, nao falha
    }
    if (removedLen > TEXT_BUFFER_SIZE) return;
    memcpy(undoRemoved, removed, removedLen);
    undoRemovedLen = removedLen;
  }
  undoInserted = inserted;
  undoPos = pos;
}
static size_t clipboardLen = 0;

// Forward declaration
static void ensureCursorVisible(int visibleLines);
static void beginOrKeepSelection() {
  if (selectionAnchor < 0) selectionAnchor = cursorPosition;
}

// Recalculate line breaks (word wrap) and cursor position.
// The O(textLength) line break loop only runs when the buffer or maxLineWidthPx changed.
// Cursor line/col is always recomputed (cheap O(cursorLine) with early exit).
//
// Wraps on measured glyph width, not a character-count estimate: an average
// char width (even a font-correct one) is only right for lines that happen
// to match the sample's mix of narrow/wide glyphs, and any line running
// wider than that average overflows the pixel budget — invisible past the
// screen edge, since drawEditorLine's drawClippedText() silently clips
// overflow instead of erroring. Summing each codepoint's real advance width
// (via glyphWidthFn, no kerning) makes the wrap decision itself pixel-exact,
// so no line can ever be wider than maxLineWidthPx to begin with.
void editorRecalculateLines() {
  if (lineBreaksDirty) {
    lineCount = 0;
    linePositions[0] = 0;
    lineCount = 1;

    int lineWidthPx = 0;  // accumulated width of the line being built
    int wordWidthPx = 0;  // width of the in-progress word, since the last space
    int lastSpace = -1;

    int i = 0;
    while (i < (int)textLength && lineCount < MAX_LINES) {
      const unsigned char c = static_cast<unsigned char>(textBuffer[i]);

      if (c == '\n') {
        // Hard line break
        linePositions[lineCount++] = i + 1;
        lineWidthPx = 0;
        wordWidthPx = 0;
        lastSpace = -1;
        i++;
        continue;
      }

      const unsigned char* p = reinterpret_cast<const unsigned char*>(&textBuffer[i]);
      const uint32_t cp = utf8NextCodepoint(&p);
      int charBytes = static_cast<int>(reinterpret_cast<const char*>(p) - &textBuffer[i]);
      if (charBytes <= 0) charBytes = 1;  // safety net against a malformed byte
      const int cw = glyphWidthFn ? glyphWidthFn(cp) : 8;

      if (c == ' ') {
        // Space: counts toward the line, never toward the next word's width.
        lastSpace = i;
        lineWidthPx += cw;
        if (lineWidthPx > maxLineWidthPx) {
          const int breakPos = i + charBytes;  // break right after this space
          linePositions[lineCount++] = breakPos;
          lineWidthPx = 0;
          wordWidthPx = 0;
          lastSpace = -1;
        } else {
          wordWidthPx = 0;
        }
      } else {
        lineWidthPx += cw;
        wordWidthPx += cw;
        if (lineWidthPx > maxLineWidthPx) {
          int breakPos;
          int carryPx;
          if (lastSpace > linePositions[lineCount - 1]) {
            breakPos = lastSpace + 1;  // break after the last space; word rolls to next line
            carryPx = wordWidthPx;     // width of what's after that space, incl. this char
          } else {
            breakPos = i + charBytes;  // no space on this line — hard break mid-word
            carryPx = 0;
          }
          linePositions[lineCount++] = breakPos;
          lineWidthPx = carryPx;
          wordWidthPx = carryPx;
          lastSpace = -1;
        }
      }

      i += charBytes;
    }
    lineBreaksDirty = false;
  }

  // Compute cursor line and column (always — cheap O(cursorLine) with early exit)
  cursorLine = 0;
  for (int i = 1; i < lineCount; i++) {
    if (cursorPosition >= linePositions[i]) {
      cursorLine = i;
    } else {
      break;
    }
  }
  cursorCol = cursorPosition - linePositions[cursorLine];
}

// Ensure cursor is visible by adjusting viewport
static void ensureCursorVisible(int visibleLines) {
  if (visibleLines <= 0) visibleLines = 20; // fallback

  if (cursorLine < viewportStartLine) {
    viewportStartLine = cursorLine;
  } else if (cursorLine >= viewportStartLine + visibleLines) {
    viewportStartLine = cursorLine - visibleLines + 1;
  }

  if (viewportStartLine < 0) viewportStartLine = 0;
  if (viewportStartLine >= lineCount) viewportStartLine = std::max(0, lineCount - 1);
}

void editorInit() {
  memset(textBuffer, 0, TEXT_BUFFER_SIZE);
  textLength = 0;
  cursorPosition = 0;
  currentFile[0] = '\0';
  strncpy(currentTitle, "Untitled", MAX_TITLE_LEN - 1);
  unsavedChanges = false;
  viewportStartLine = 0;
  selectionAnchor = -1;
  lineBreaksDirty = true;
  editorRecalculateLines();
}

void editorClear() {
  memset(textBuffer, 0, TEXT_BUFFER_SIZE);
  textLength = 0;
  cursorPosition = 0;
  unsavedChanges = false;
  viewportStartLine = 0;
  selectionAnchor = -1;
  lineBreaksDirty = true;
  editorRecalculateLines();
}

void editorLoadBuffer(size_t length) {
  textLength = length;
  textBuffer[textLength] = '\0';
  cursorPosition = (int)textLength;  // Start at end
  viewportStartLine = 0;
  selectionAnchor = -1;
  lineBreaksDirty = true;
  editorRecalculateLines();
  // Scroll to show cursor
  ensureCursorVisible(storedVisibleLines);
}

char* editorGetBuffer() { return textBuffer; }
size_t editorGetLength() { return textLength; }
int editorGetCursorPosition() { return cursorPosition; }

int editorGetWordCount() {
  int count = 0;
  bool inWord = false;
  for (size_t i = 0; i < textLength; i++) {
    char c = textBuffer[i];
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
      inWord = false;
    } else {
      if (!inWord) { count++; inWord = true; }
    }
  }
  return count;
}

void editorInsertChar(char c) {
  undoClear();  // edicao fora de bloco: ver editorUndo em text_editor.h
  if (textLength >= TEXT_BUFFER_SIZE - 1) return;

  // Shift text right
  for (int i = (int)textLength; i > cursorPosition; i--) {
    textBuffer[i] = textBuffer[i - 1];
  }
  textBuffer[cursorPosition] = c;
  cursorPosition++;
  textLength++;
  textBuffer[textLength] = '\0';
  unsavedChanges = true;
  lineBreaksDirty = true;

  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

// Insert a UTF-8 encoded string (one or more bytes) at the cursor position.
// Each byte is inserted individually so the existing single-char shift logic
// is reused; the renderer already iterates codepoints via utf8NextCodepoint.
void editorInsertUtf8(const char* utf8str) {
  if (!utf8str) return;
  for (const char* p = utf8str; *p != '\0'; p++) {
    editorInsertChar(*p);
  }
}

// UTF-8 helpers for deletion
// Returns the number of bytes of the codepoint that *ends* at position `pos-1`.
//
// FIX: a versão original testava textBuffer[pos - len - 1], pulando o byte
// imediatamente anterior e nunca detectando bytes de continuação (10xxxxxx).
// A correção testa textBuffer[pos - len], recuando corretamente sobre todos
// os bytes de continuação antes de parar no lead byte.
static int utf8BackLen(int pos) {
  if (pos <= 0) return 0;
  int len = 1;
  // Recua sobre bytes de continuação (10xxxxxx = 0x80..0xBF)
  while (len < 4 && (pos - len) > 0 &&
         (static_cast<unsigned char>(textBuffer[pos - len]) & 0xC0) == 0x80) {
    len++;
  }
  return len;
}

// Returns the number of bytes of the codepoint that *starts* at position `pos`.
static int utf8FwdLen(int pos) {
  if (pos >= (int)textLength) return 0;
  unsigned char c = static_cast<unsigned char>(textBuffer[pos]);
  if (c < 0x80) return 1;
  if ((c >> 5) == 0x6) return 2;
  if ((c >> 4) == 0xE) return 3;
  if ((c >> 3) == 0x1E) return 4;
  return 1;  // fallback for stray continuation byte
}

void editorDeleteChar() {
  undoClear();  // edicao fora de bloco: ver editorUndo em text_editor.h
  if (cursorPosition <= 0 || textLength == 0) return;

  int len = utf8BackLen(cursorPosition);  // bytes to remove (1–4)
  int newPos = cursorPosition - len;

  for (int i = newPos; i < (int)textLength - len; i++) {
    textBuffer[i] = textBuffer[i + len];
  }
  cursorPosition = newPos;
  textLength -= len;
  textBuffer[textLength] = '\0';
  unsavedChanges = true;
  lineBreaksDirty = true;

  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorDeleteForward() {
  undoClear();  // edicao fora de bloco: ver editorUndo em text_editor.h
  if (cursorPosition >= (int)textLength) return;

  int len = utf8FwdLen(cursorPosition);  // bytes to remove (1–4)

  for (int i = cursorPosition; i < (int)textLength - len; i++) {
    textBuffer[i] = textBuffer[i + len];
  }
  textLength -= len;
  textBuffer[textLength] = '\0';
  unsavedChanges = true;
  lineBreaksDirty = true;

  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorMoveCursorLeft(bool extendSelection) {
  if (extendSelection) beginOrKeepSelection(); else editorClearSelection();
  if (cursorPosition > 0) {
    int len = utf8BackLen(cursorPosition);
    cursorPosition -= len;
    editorRecalculateLines();
    ensureCursorVisible(storedVisibleLines);
  }
}

void editorMoveCursorRight(bool extendSelection) {
  if (extendSelection) beginOrKeepSelection(); else editorClearSelection();
  if (cursorPosition < (int)textLength) {
    int len = utf8FwdLen(cursorPosition);
    cursorPosition += len;
    editorRecalculateLines();
    ensureCursorVisible(storedVisibleLines);
  }
}

void editorMoveCursorUp(bool extendSelection) {
  if (extendSelection) beginOrKeepSelection(); else editorClearSelection();
  // cursorLine/cursorCol are already valid from the previous operation
  if (cursorLine <= 0) return;

  int targetLine = cursorLine - 1;
  int lineStart = linePositions[targetLine];
  int lineEnd = (targetLine + 1 < lineCount) ? linePositions[targetLine + 1] : (int)textLength;
  int lineLen = lineEnd - lineStart;
  // Don't count trailing newline
  if (lineLen > 0 && textBuffer[lineStart + lineLen - 1] == '\n') lineLen--;

  cursorPosition = lineStart + std::min(cursorCol, lineLen);
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorMoveCursorDown(bool extendSelection) {
  if (extendSelection) beginOrKeepSelection(); else editorClearSelection();
  if (cursorLine >= lineCount - 1) return;

  int targetLine = cursorLine + 1;
  int lineStart = linePositions[targetLine];
  int lineEnd = (targetLine + 1 < lineCount) ? linePositions[targetLine + 1] : (int)textLength;
  int lineLen = lineEnd - lineStart;
  if (lineLen > 0 && textBuffer[lineStart + lineLen - 1] == '\n') lineLen--;

  cursorPosition = lineStart + std::min(cursorCol, lineLen);
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorMoveCursorHome(bool extendSelection) {
  if (extendSelection) beginOrKeepSelection(); else editorClearSelection();
  cursorPosition = linePositions[cursorLine];
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorMoveCursorEnd(bool extendSelection) {
  if (extendSelection) beginOrKeepSelection(); else editorClearSelection();
  int lineEnd;
  if (cursorLine + 1 < lineCount) {
    lineEnd = linePositions[cursorLine + 1];
    // Step back over newline if present
    if (lineEnd > 0 && textBuffer[lineEnd - 1] == '\n') lineEnd--;
  } else {
    lineEnd = (int)textLength;
  }
  cursorPosition = lineEnd;
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

// --- Selection & clipboard ---

void editorClearSelection() { selectionAnchor = -1; }

void editorSelectAll() {
  if (textLength == 0) return;
  selectionAnchor = 0;
  cursorPosition = (int)textLength;
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

bool editorHasSelection() { return selectionAnchor >= 0 && selectionAnchor != cursorPosition; }

int editorGetSelectionStart() {
  if (!editorHasSelection()) return -1;
  return std::min(selectionAnchor, cursorPosition);
}

int editorGetSelectionEnd() {
  if (!editorHasSelection()) return -1;
  return std::max(selectionAnchor, cursorPosition);
}

void editorDeleteSelection() {
  if (!editorHasSelection()) return;
  int start = editorGetSelectionStart();
  int end = editorGetSelectionEnd();
  int len = end - start;
  undoRecord(start, textBuffer + start, (size_t)len, 0);
  memmove(textBuffer + start, textBuffer + end, textLength - end);
  textLength -= len;
  textBuffer[textLength] = '\0';
  cursorPosition = start;
  selectionAnchor = -1;
  unsavedChanges = true;
  lineBreaksDirty = true;
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorCopySelection() {
  if (!editorHasSelection()) return;
  int start = editorGetSelectionStart();
  size_t len = (size_t)(editorGetSelectionEnd() - start);
  if (!clipboard) {
    clipboard = (char*)malloc(TEXT_BUFFER_SIZE);
    if (!clipboard) return;  // nothing copied; the selection is untouched
  }
  memcpy(clipboard, textBuffer + start, len);
  clipboard[len] = '\0';
  clipboardLen = len;
}

void editorCutSelection() {
  editorCopySelection();
  editorDeleteSelection();  // e quem grava o registro de desfazer
}

bool editorHasClipboardContent() { return clipboardLen > 0; }

bool editorCanUndo() { return undoPos >= 0; }

void editorUndo() {
  if (undoPos < 0) return;
  const int pos = undoPos;
  const size_t ins = undoInserted;
  const size_t rem = undoRemovedLen;

  // Ordem inversa da operacao: tira o que entrou, devolve o que saiu.
  if (ins > 0 && pos + (int)ins <= textLength) {
    memmove(textBuffer + pos, textBuffer + pos + ins, textLength - pos - ins);
    textLength -= ins;
  }
  if (rem > 0 && undoRemoved && textLength + rem < TEXT_BUFFER_SIZE) {
    memmove(textBuffer + pos + rem, textBuffer + pos, textLength - pos);
    memcpy(textBuffer + pos, undoRemoved, rem);
    textLength += rem;
  }
  textBuffer[textLength] = '\0';
  cursorPosition = pos + (int)rem;
  if (cursorPosition > textLength) cursorPosition = textLength;
  selectionAnchor = -1;

  // Um nivel so: desfazer consome o registro. Sem redo, e sem a armadilha de
  // um Ctrl+Z repetido refazer a operacao por engano.
  undoClear();

  unsavedChanges = true;
  lineBreaksDirty = true;
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

// One shift + one line-break recompute, unlike editorInsertUtf8 (byte-at-a-
// time, fine for the 1-4 bytes a dead key composes, but would be
// O(pastedBytes * textLength) for a multi-line paste).
// Cola substituindo a selecao, se houver, como UMA operacao. O chamador nao
// deve apagar a selecao antes: dois registros separados fariam o Ctrl+Z
// desfazer so a colagem, deixando o trecho apagado perdido.
void editorPasteOverSelection() {
  if (clipboardLen == 0 || !clipboard) return;
  if (!editorHasSelection()) { editorPasteAtCursor(); return; }

  const int start = editorGetSelectionStart();
  const int end = editorGetSelectionEnd();
  const size_t removedLen = (size_t)(end - start);

  size_t len = clipboardLen;
  if (textLength - removedLen + len >= TEXT_BUFFER_SIZE) {
    len = TEXT_BUFFER_SIZE - 1 - (textLength - removedLen);
  }
  undoRecord(start, textBuffer + start, removedLen, len);

  // remove a selecao sem gravar registro proprio, e cola sem gravar tambem
  memmove(textBuffer + start, textBuffer + end, textLength - end);
  textLength -= removedLen;
  cursorPosition = start;
  selectionAnchor = -1;
  if (len > 0) {
    memmove(textBuffer + cursorPosition + len, textBuffer + cursorPosition, textLength - cursorPosition);
    memcpy(textBuffer + cursorPosition, clipboard, len);
    textLength += len;
    cursorPosition += (int)len;
  }
  textBuffer[textLength] = '\0';
  unsavedChanges = true;
  lineBreaksDirty = true;
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorPasteAtCursor() {
  if (clipboardLen == 0 || !clipboard) return;
  size_t len = clipboardLen;
  if (textLength + len >= TEXT_BUFFER_SIZE) len = TEXT_BUFFER_SIZE - 1 - textLength;
  if (len == 0) return;
  undoRecord(cursorPosition, nullptr, 0, len);
  memmove(textBuffer + cursorPosition + len, textBuffer + cursorPosition, textLength - cursorPosition);
  memcpy(textBuffer + cursorPosition, clipboard, len);
  textLength += len;
  cursorPosition += (int)len;
  textBuffer[textLength] = '\0';
  unsavedChanges = true;
  lineBreaksDirty = true;
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorSetMaxLineWidthPx(int px) {
  if (px != maxLineWidthPx) {
    maxLineWidthPx = px;
    lineBreaksDirty = true;
  }
  editorRecalculateLines();
}

void editorSetGlyphWidthFn(int (*fn)(uint32_t)) {
  if (fn != glyphWidthFn) {
    glyphWidthFn = fn;
    lineBreaksDirty = true;
  }
}

void editorSetVisibleLines(int n) {
  if (n > 0) storedVisibleLines = n;
}

int editorGetStoredVisibleLines() {
  return storedVisibleLines;
}

void editorSetPageJumpLines(int n) {
  if (n > 0) pageJumpLines = n;
}

int editorGetPageJumpLines() {
  return pageJumpLines;
}

int editorGetVisibleLines(int lineHeight, int textAreaHeight) {
  if (lineHeight <= 0) return 20;
  return textAreaHeight / lineHeight;
}

int editorGetViewportStart() { return viewportStartLine; }
int editorGetCursorLine() { return cursorLine; }
int editorGetCursorCol() { return cursorCol; }
int editorGetLineCount() { return lineCount; }

int editorGetLinePosition(int lineIndex) {
  if (lineIndex < 0 || lineIndex >= lineCount) return 0;
  return linePositions[lineIndex];
}

void editorSetCurrentFile(const char* filename) {
  strncpy(currentFile, filename, MAX_FILENAME_LEN - 1);
  currentFile[MAX_FILENAME_LEN - 1] = '\0';
}

void editorSetCurrentTitle(const char* title) {
  strncpy(currentTitle, title, MAX_TITLE_LEN - 1);
  currentTitle[MAX_TITLE_LEN - 1] = '\0';
}

const char* editorGetCurrentFile() { return currentFile; }
const char* editorGetCurrentTitle() { return currentTitle; }
bool editorHasUnsavedChanges() { return unsavedChanges; }
void editorSetUnsavedChanges(bool v) { unsavedChanges = v; }
