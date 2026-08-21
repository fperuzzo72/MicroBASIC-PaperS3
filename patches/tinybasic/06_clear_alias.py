"""Acrescenta CLEAR como segunda grafia de CLR (basic.c).

Este interpretador seguiu a linhagem Commodore/Apple e chamou de CLR o comando
que limpa variaveis. MS-BASIC, MSX e Spectrum chamavam de CLEAR, e e esse o
nome que vem a cabeca primeiro. As duas grafias passam a existir.

Como funciona: o lexer varre keyword[] em ordem e casa a primeira palavra que
seja prefixo da entrada, devolvendo o token de tokens[] no mesmo indice. Dois
indices com o mesmo token sao, portanto, dois nomes para o mesmo comando.

Ordem importa, por dois motivos:

1. CLEAR entra *depois* de CLR. "CLR" nao e prefixo de "CLEAR" (diferem no
   terceiro caractere), entao nenhuma das duas rouba a entrada da outra -- mas
   manter CLR primeiro fixa qual delas e a canonica.
2. LIST e HELP procuram o *primeiro* indice com um dado token para imprimir o
   nome. Com CLR na frente, um programa listado sai com CLR mesmo que tenha
   sido digitado CLEAR, e a listagem nao vira uma mistura das duas.
"""

PATH = "editor/lib/TinyBasic/basic.c"
src = open(PATH).read()

assert '"CLEAR"' not in src, "basic.c: CLEAR ja existe"

# 1. A string da palavra-chave, ao lado da de CLR.
OLD_STR = 'const char sclr[]    PROGMEM = "CLR";'
assert src.count(OLD_STR) == 1, "basic.c: sclr[] nao encontrado (ou mudou)"
src = src.replace(
    OLD_STR,
    OLD_STR + '\n/* MicroBASIC: segunda grafia, ver patches/tinybasic/06 */\n'
              'const char sclear[]  PROGMEM = "CLEAR";',
    1,
)

# 2. keyword[] e tokens[]: mesma posicao nas duas tabelas, logo apos CLR.
OLD_KW = "  sclr, shimem, stab, sthen,"
assert src.count(OLD_KW) == 1, "basic.c: entrada de sclr em keyword[] nao encontrada"
src = src.replace(OLD_KW, "  sclr, sclear, shimem, stab, sthen,", 1)

OLD_TK = "  TNOT, TAND, TOR, TLEN, TSGN, TPEEK, TDIM, TCLR,"
assert src.count(OLD_TK) == 1, "basic.c: entrada de TCLR em tokens[] nao encontrada"
src = src.replace(OLD_TK, OLD_TK + " TCLR,", 1)

open(PATH, "w").write(src)
print("basic.c: CLEAR aceito como CLR")
