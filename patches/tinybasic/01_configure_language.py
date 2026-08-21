"""Patch em editor/lib/TinyBasic/language.h: define explicitamente o conjunto
de recursos da linguagem em vez de deixar a heurística do upstream escolher
pela placa.

Por quê: as heurísticas dele olham o tamanho de flash/RAM da placa e escolhem
um perfil (BASICFULL/BASICSIMPLE/...). No ESP32-C3 isso ligaria tudo --
inclusive Wi-Fi/MQTT, sensores, câmera e I/O de pinos -- e cada um desses
recursos arrasta funções para o contrato de runtime que teríamos de
implementar ou stubar sem nunca usar. Medido: desligar os blocos abaixo tira
8 funções do contrato e uma quantidade grande de código morto.

O que fica ligado é o que faz o MicroBASIC parecer o BASIC de linha numerada
que ele quer ser -- em especial HASMSSTRINGS (LEFT$/RIGHT$/MID$/ASC/CHR$ e
concatenação com +) e HASDARTMOUTH (DEF FN, ON ... GOTO/GOSUB, READ/DATA).
"""

PATH = "editor/lib/TinyBasic/language.h"

# (flag, mantém?, por quê)
FEATURES = [
    ("HASAPPLE1",             True,  "base: heap, strings, arrays -- exigido pelo resto"),
    ("HASFILEIO",             True,  "SAVE/LOAD no cartão SD"),
    ("HASSTEFANSEXT",         True,  "ELSE, FOR avançado, SQR, POW"),
    ("HASERRORMSG",           True,  "mensagens de erro em texto, nao so codigo"),
    ("HASFLOAT",              True,  "ponto flutuante"),
    ("HASDARTMOUTH",          True,  "DEF FN, ON..GOTO/GOSUB, READ/DATA -- classicos"),
    ("HASMULTIDIM",           True,  "arrays 2D e arrays de string"),
    ("HASERRORHANDLING",      True,  "ERROR GOTO"),
    ("HASSTRUCT",             True,  "WHILE/WEND, REPEAT/UNTIL, SWITCH/CASE"),
    ("HASMSSTRINGS",          True,  "LEFT$/RIGHT$/MID$/ASC/CHR$ -- essencial p/ MSX"),
    ("HASMULTILINEFUNCTIONS", True,  "DEF FN multilinha"),
    ("HASEDITOR",             True,  "editor de linha do console"),
    ("HASTINYBASICINPUT",     True,  "INPUT aceitando expressoes"),
    ("HASLONGNAMES",          True,  "nomes de variavel com ate 16 chars"),
    ("HASHELP",               True,  "lista de comandos"),
    ("HASFULLINSTR",          True,  "INSTR completo estilo C64"),
    ("HASLOOPOPT",            True,  "FOR otimizado"),
    ("HASNUMSYSTEM",          True,  "constantes hex/octal/binario"),

    ("HASARDUINOIO",          False, "I/O de pinos -- sem uso aqui, arrasta ~10 funcoes"),
    ("HASTONE",               False, "PLAY/tone -- sem buzzer"),
    ("HASPULSE",              False, "PULSE -- sem uso"),
    ("HASGRAPH",              False, "LINE/CIRCLE/etc -- reativar junto com SCREEN 4"),
    ("HASDARKARTS",           False, "MALLOC/FIND/EVAL -- automodificacao, nao queremos"),
    ("HASIOT",                False, "Wi-Fi, MQTT, sensores -- o Wi-Fi aqui e do wifi_sync"),
    ("HASTIMER",              False, "AFTER/EVERY"),
    ("HASEVENTS",             False, "EVENT"),
    ("HASCAMERA",             False, "esp32cam"),
]

src = open(PATH).read()

# 1. Desliga a heuristica para que o bloco de ajuste manual passe a valer.
old = "#define LANGUAGEHEURISTICS"
assert old in src, "language.h: #define LANGUAGEHEURISTICS nao encontrado"
src = src.replace(
    old,
    "/* MicroBASIC: conjunto de recursos definido explicitamente abaixo, nao\n"
    "   pela heuristica de tamanho de placa do upstream. */\n"
    "/* #define LANGUAGEHEURISTICS */",
    1,
)

# 2. Comenta os recursos que nao queremos, dentro do bloco manual.
for flag, keep, why in FEATURES:
    if keep:
        continue
    needle = "#define %s\n" % flag
    assert needle in src, "language.h: %s nao encontrado" % flag
    src = src.replace(needle, "/* #define %s */  /* MicroBASIC: %s */\n" % (flag, why), 1)

open(PATH, "w").write(src)

kept = sum(1 for _, k, _ in FEATURES if k)
print("language.h: OK (%d recursos ligados, %d desligados)" % (kept, len(FEATURES) - kept))
