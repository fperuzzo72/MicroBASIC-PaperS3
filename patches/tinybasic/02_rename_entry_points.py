"""Patch em editor/lib/TinyBasic/basic.c: renomeia setup()/loop() e remove o
main() do upstream.

Por quê: o interpretador foi escrito para *ser* o programa -- expõe setup() e
loop() no estilo Arduino e, fora do Arduino, um main() que chama os dois. Aqui
ele é uma biblioteca dentro de um firmware que já tem o seu próprio setup() e
loop() (editor/src/main.cpp), então os símbolos colidiriam no link.

Renomeados para basicSetup()/basicLoop(), que é como editor/src/tb_runtime.cpp
os chama. O main() do upstream é removido de vez: mesmo protegido por
#ifndef ARDUINO (e portanto já inativo neste build), deixá-lo só cria a
possibilidade de alguém mudar uma flag e ganhar dois main() sem entender por quê.
"""

import re

PATH = "editor/lib/TinyBasic/basic.c"

src = open(PATH).read()

# 1. setup() -> basicSetup()
old_setup = "void setup() {"
assert old_setup in src, "basic.c: void setup() nao encontrado"
src = src.replace(
    old_setup,
    "/* MicroBASIC: renomeado de setup() -- ver patches/tinybasic/. */\n"
    "void basicSetup() {",
    1,
)

# 2. loop() -> basicLoop()
old_loop = "void loop() {"
assert old_loop in src, "basic.c: void loop() nao encontrado"
src = src.replace(
    old_loop,
    "/* MicroBASIC: renomeado de loop() -- ver patches/tinybasic/. */\n"
    "void basicLoop() {",
    1,
)

# 3. Remove o main() do upstream junto com o #ifndef ARDUINO que o cerca.
#    Cuidado: há um `#ifdef HASARGS ... #endif` aninhado *dentro* do main, então
#    o primeiro #endif encontrado não é o do bloco -- é preciso contar
#    aninhamento até fechar o #ifndef externo. (Cortar no primeiro #endif deixa
#    metade do main solta no arquivo e o erro só aparece depois, como
#    "expected identifier before 'while'".)
start = src.find("#ifndef ARDUINO\nint main(int argc, char* argv[]) {")
assert start >= 0, "basic.c: bloco do main() nao encontrado"

depth = 0
pos = start
end = -1
for m in re.finditer(r"^[ \t]*#\s*(ifn?def|if|endif)\b", src[start:], re.M):
    kind = m.group(1)
    if kind == "endif":
        depth -= 1
        if depth == 0:
            end = start + m.end()
            break
    else:
        depth += 1
assert end > 0, "basic.c: #endif correspondente ao bloco do main() nao encontrado"
src = (
    src[:start]
    + "/* MicroBASIC: main() do upstream removido -- aqui o interpretador e uma\n"
    "   biblioteca, quem tem main/setup/loop e o firmware (editor/src/main.cpp). */"
    + src[end:]
)

open(PATH, "w").write(src)
print("basic.c: OK (setup/loop renomeados, main() removido)")
