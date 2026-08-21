"""Corrige um off-by-one em clrvars() (basic.c), upstream.

CLR limpa as variaveis e deixa exatamente um byte para tras -- o do endereco
mais alto do heap, que pertence a *primeira* variavel alocada. Little-endian,
esse e o byte mais significativo de um float, entao o efeito e visivel e
estranho:

    A=5   -> 2        A=100 -> 32
    A=8   -> 8        A=3.5 -> 2
    A=1   -> 0.5

Sao exatamente os valores de zerar tres dos quatro bytes e manter o do topo.
Conferido bit a bit; ver docs/DEVELOPMENT_LOG.md para o levantamento.

A causa: o heap de variaveis cresce para baixo a partir de memsize, logo o
ultimo byte do objeto mais alto fica no indice memsize -- que `i < memsize`
exclui. Um caractere resolve.

Por que corrigir isto e nao o CONT (tambem upstream): este e trivial,
localizado, e sem qualquer efeito colateral -- so alcanca bytes que a propria
funcao ja pretendia zerar. O do CONT exige salvar e restaurar estado do
interpretador, que e outra ordem de risco.
"""

PATH = "editor/lib/TinyBasic/basic.c"
src = open(PATH).read()

OLD = "  for (i = himem; i < memsize; i++) memwrite2(i, 0);"
NEW = ("  /* MicroBASIC: <= e nao <. O heap cresce para baixo a partir de memsize,\n"
       "     entao o ultimo byte do objeto no topo fica *em* memsize e sobrevivia.\n"
       "     Ver patches/tinybasic/05 e docs/DEVELOPMENT_LOG.md. */\n"
       "  for (i = himem; i <= memsize; i++) memwrite2(i, 0);")

assert src.count(OLD) == 1, "basic.c: laco de clrvars() nao encontrado (ou mudou)"
src = src.replace(OLD, NEW, 1)
open(PATH, "w").write(src)
print("basic.c: clrvars() limpa ate memsize inclusive")
