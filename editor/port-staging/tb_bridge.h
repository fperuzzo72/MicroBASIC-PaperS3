#pragma once

// Bridge between the SCREEN 0-3 terminal and Stefan Lenz's IoT BASIC
// interpreter (editor/lib/TinyBasic, see patches/tinybasic/).
//
// The interpreter is written as its own REPL: basicLoop() prints a prompt,
// *blocks* in ins() until a line arrives, then either stores it (if it starts
// with a number) or executes it. That shape doesn't fit here -- input on this
// device arrives asynchronously, keystroke by keystroke, from the BLE host
// task into a queue, and line editing is the screen editor's job (free cursor
// movement, wrapped logical lines, Enter reading whichever line the cursor is
// on).
//
// So basicLoop() is not used. tbExecuteLine() instead does exactly what
// basicLoop() does *after* its ins() call -- inject the finished line into the
// interpreter's input buffer, tokenise, and dispatch -- which keeps the
// interpreter's own semantics while leaving line editing entirely to us.

void tbSetup();

// Runs one line as typed, exactly as the interpreter's own REPL would have:
// a line starting with a number goes to its program memory, anything else
// executes immediately. Output lands on the terminal through the runtime's
// outch() (editor/src/tb_runtime.cpp).
//
// Returns false if the interpreter reported an error; the error text has
// already been printed to the terminal by then.
bool tbExecuteLine(const char* line);

// True while the interpreter has control -- RUN, LIST, CATALOG, any direct
// statement. The terminal cursor is hidden for the duration: a cursor means
// "waiting for you to type", and nothing is. It was showing up next to
// whatever a program printed last, which on a game repainting cells in place
// meant a block permanently stuck to the sprite.
bool tbIsRunning();

// Silences the runtime's file-failure reporting. The interpreter probes for
// an autoexec.bas at startup with `if (ifileopen(...))`, so a failure there
// is the normal case, not a fault worth announcing on the boot screen.
void tbRuntimeSetQuiet(bool quiet);

// Desliga a execucao do autoexec.bas neste boot. Chamado antes de tbSetup()
// quando o usuario segura BACK no arranque.
void tbSetAutoexecEnabled(bool enabled);

// Executa o autoexec.bas que basicSetup() carregou, se houver. Chamado do
// loop(), NAO do setup(): um lancador roda para sempre, e rodando dentro do
// setup() o loop() nunca comeca -- que e quem le o botao de power, desenha a
// tela e mantem a interface viva. Ver docs/DEVELOPMENT_LOG.md.
// Retorna true se havia algo para rodar.
bool tbRunPendingAutoexec();
