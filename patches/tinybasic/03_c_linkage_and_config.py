"""Patches em editor/lib/TinyBasic/{runtime.h,basic.h,hardware.h}:

1. Guards extern "C" em runtime.h e basic.h. O interpretador é C puro; o nosso
   runtime (editor/src/tb_runtime.cpp) é C++, porque precisa chamar o editor de
   tela e o SDCardManager, que são C++. Sem os guards, o C++ decoraria os nomes
   e nada linkaria contra o basic.c compilado como C.

2. Ajustes de hardware.h para este alvo: sem os periféricos do upstream (o
   display, o teclado e o sistema de arquivos aqui são os do firmware, expostos
   pelo nosso runtime) e com o tamanho da memória BASIC fixado.
"""

import re

# --- 1. extern "C" ---------------------------------------------------------
for path in ("editor/lib/TinyBasic/runtime.h", "editor/lib/TinyBasic/basic.h"):
    src = open(path).read()
    assert 'extern "C"' not in src, "%s: ja tem guards" % path
    src = (
        '/* MicroBASIC: o interpretador compila como C, o nosso runtime como\n'
        '   C++ (precisa falar com o editor de tela e o SD, ambos C++). Sem\n'
        '   isto os nomes nao batem no link. Ver patches/tinybasic/. */\n'
        '#ifdef __cplusplus\nextern "C" {\n#endif\n\n' + src +
        '\n#ifdef __cplusplus\n}\n#endif\n'
    )
    open(path, "w").write(src)
    print("%s: OK (extern \"C\")" % path.split("/")[-1])

# --- 2. hardware.h ---------------------------------------------------------
PATH = "editor/lib/TinyBasic/hardware.h"
src = open(PATH).read()

# Periféricos do upstream que este firmware já resolve por conta própria.
# Cada um destes, se ligado, puxa drivers e amplia o contrato de runtime.
for flag, why in [
    ("HASBUILDIN", "programas de exemplo em PROGMEM -- nao usamos"),
    ("HASCAMERA", "esp32cam"),
]:
    needle = "#define %s\n" % flag
    if needle in src:
        src = src.replace(needle, "/* #define %s */  /* MicroBASIC: %s */\n" % (flag, why), 1)
        print("hardware.h: %s desligado" % flag)

# A variante Posix declara à mão os tipos que o Arduino normalmente fornece
# (uint8_t, uint32_t, byte...), porque num POSIX puro eles não vêm de lugar
# nenhum. Aqui vêm: este firmware compila com Arduino.h/stdint.h, e as
# declarações do upstream colidem com as reais ("conflicting declaration
# 'typedef unsigned int uint32_t'"). Trocadas pelo stdint.h de verdade,
# mantendo só `byte`, que é do Arduino e não do C padrão.
old_types = """typedef unsigned char uint8_t;
typedef signed char int8_t;
typedef unsigned short uint16_t;
typedef signed short int16_t;
typedef unsigned char byte;
typedef unsigned long long uint64_t;
typedef unsigned int uint32_t;"""
new_types = """/* MicroBASIC: tipos vindos do stdint.h real em vez de declarados a mao --
   este alvo tem Arduino.h/stdint.h e as declaracoes do upstream colidiam
   ("conflicting declaration 'typedef unsigned int uint32_t'").

   `byte` continua sendo declarado aqui, inclusive em C++, e de proposito:
   runtime.h usa `byte` numa assinatura (mqttcallback) e nao ha garantia de
   que quem incluir este header ja tenha incluido Arduino.h antes. Como o
   Arduino define exatamente `typedef uint8_t byte;`, repetir a MESMA typedef
   e legal em C++ e vale em qualquer ordem de include -- ao contrario de
   deixar sob #ifndef __cplusplus, que quebrava o segundo .cpp a incluir isto
   (tb_bridge.cpp: "'byte' has not been declared").
   Ver patches/tinybasic/. */
#include <stdint.h>
typedef uint8_t byte;"""
assert old_types in src, "hardware.h: bloco de typedefs de compatibilidade nao encontrado"
src = src.replace(old_types, new_types, 1)
print("hardware.h: typedefs trocados por stdint.h")

# O upstream declara millis() como `unsigned long millis();` para POSIX. No
# Arduino ela ja vem declarada e a assinatura difere; deixar a declaracao dele
# gera conflito.
# millis(): a declaracao FICA. basic.c compila como C puro e nunca inclui
# Arduino.h, entao sem ela da "implicit declaration of function 'millis'".
# A assinatura bate com a do Arduino (unsigned long millis(void)), entao
# conviver com as duas nao gera conflito.

# Tamanho da memória BASIC. 0 = "descubra sozinho", que no upstream significa
# tentar alocar quase toda a RAM livre -- exatamente o que nao pode acontecer
# aqui: o BLE precisa de um bloco contiguo de 20KB no momento da conexao, e ja
# ficamos do lado errado disso uma vez (ver docs/DEVELOPMENT_LOG.md). Fixado
# num valor que deixa folga de sobra.
m = re.search(r"^#define MEMSIZE .*$", src, re.M)
assert m, "hardware.h: #define MEMSIZE nao encontrado"
src = src[:m.start()] + (
    "/* MicroBASIC: fixo, nao 0 (=auto). O auto do upstream toma quase toda a\n"
    "   RAM livre; aqui o BLE precisa de 20KB contiguos ao conectar. */\n"
    "#define MEMSIZE 16384"
) + src[m.end():]
print("hardware.h: MEMSIZE fixado em 16384")

open(PATH, "w").write(src)
