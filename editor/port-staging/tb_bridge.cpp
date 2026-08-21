#include "tb_bridge.h"

#include "config.h"
#include "screen_editor.h"
#include "input_handler.h"

// Definidos em main.cpp. Chamados sempre que o interpretador devolve o
// controle: o toque que encerrou o programa nao pode virar uma segunda borda
// na interface, nem sobrar na fila de teclas.
void physicalButtonsRearm();

extern bool screenDirty;

#include <cstring>

#include "tb_interp.h"

static bool ready = false;
static bool autoexecEnabled = true;

void tbSetAutoexecEnabled(bool enabled) { autoexecEnabled = enabled; }
static bool running = false;

bool tbIsRunning() { return running; }

void tbSetup() {
  if (ready) return;
  // Printed before basicSetup() so it lands above the interpreter's own
  // multi-line banner, giving the boot screen the shape a 1980s machine had:
  // whose computer it is first, which BASIC second.
  screenEditorTermPrintLine("FSP MicroBASIC v0.3 for XTeink X4");
  // Depois do cabecalho, nao antes: a primeira linha da tela e a
  // identificacao da maquina, e um aviso de arranque vem abaixo dela.
  if (!autoexecEnabled) screenEditorTermPrintLine("Skipping autoexec.bas (BACK held)");
  // basicSetup() probes for an autoexec.bas -- `if (ifileopen(...))`, where
  // failing is the ordinary case. Without this, every boot without one
  // printed a two-line file-failure report above the interpreter's banner.
  tbRuntimeSetQuiet(true);
  basicSetup();
  tbRuntimeSetQuiet(false);
  ready = true;

  // basicSetup() sets st = SRUN when it found an autoexec.bas and loaded it.
  // Upstream runs that at the top of basicLoop(), which this bridge replaces
  // -- so without these four lines the file loads and then just sits there,
  // which is what it did until someone noticed the boot screen complaining
  // about its absence. Same three statements as upstream's, in the same
  // order. (SERUN is the EEPROM path and cannot happen here: no EEPROM.)
  // NAO executa aqui. basicSetup() deixa st = SRUN quando achou um
  // autoexec.bas; roda-lo neste ponto significa que o loop() nunca comeca,
  // e o loop() e quem le o power, desenha e mantem a interface viva. Um
  // lancador roda para sempre, entao isso deixava o aparelho aceso, mudo e
  // sem como desligar. Quem chama e o loop(), depois do primeiro desenho.
}

bool tbRunPendingAutoexec() {
  if (st != SRUN) return false;
  if (!autoexecEnabled) { top = 0; st = SINT; return false; }
  here = 0;
  xrun();
  st = SINT;
  if (er != 0) reseterror();
  physicalButtonsRearm();
  inputDiscardPendingKeys();
  screenDirty = true;
  return true;
}

bool tbExecuteLine(const char* line) {
  if (!ready) tbSetup();
  if (!line) return true;

  // ibuffer is length-prefixed, not NUL-terminated-at-zero: ins() writes the
  // length into [0] and the text from [1], with a NUL after it. Anything that
  // fills this buffer has to follow that convention or the tokeniser reads
  // the first character as a count. (Discovered from consins() in upstream's
  // own runtime, not from documentation.)
  size_t len = strlen(line);
  if (len > BUFSIZE - 3) len = BUFSIZE - 3;
  ibuffer[0] = (char)(unsigned char)len;
  memcpy(ibuffer + 1, line, len);
  ibuffer[len + 1] = '\0';

  // From here down this mirrors basicLoop() after its ins() call. Deliberately
  // kept in the same order and with the same assignments, so that when
  // upstream changes its REPL this is easy to diff against.
  // Whatever was typed before this command was typed at the editor, not at
  // the program this command may be about to start. Without this, pressing a
  // key while the previous RUN was busy would be delivered as the first move
  // of the next one.
  inputFlushProgramKeys();

  iodefaults();
  form = 0;

  bi = ibuffer;
  running = true;
  nexttoken();

  if (token == NUMBER) {
    // A numbered line is program text, not a command: it goes to the
    // interpreter's own tokenised program memory.
    ax = x;
    storeline();
  } else {
    statement();
    st = SINT;
  }

  running = false;
  physicalButtonsRearm();
  inputDiscardPendingKeys();

  screenDirty = true;
  const bool failed = (er != 0);
  if (failed) {
    // The interpreter has already printed its message through outch() by now;
    // this just clears the flag so the next line starts clean.
    reseterror();
  }

  return !failed;
}
