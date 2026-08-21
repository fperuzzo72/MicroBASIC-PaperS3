"""Cria editor/lib/TinyBasic/library.json.

Este projeto escala vários warnings para erro (ver editor/platformio.ini). Isso
é bom para o nosso código e péssimo para código de terceiros, que foi escrito
contra outro conjunto de flags: o basic.c do upstream tem labels não usadas,
fallthrough implícito e conversões que o nosso build recusaria.

Mesma decisão já tomada para o My-Basic (editor/lib/MyBasic/my_basic_impl.c):
não corrigir código de terceiros para satisfazer um nível de warning que ele
nunca prometeu atender -- isolar a biblioteca. A diferença é que aqui dá para
fazer pelo library.json do PlatformIO, que aplica as flags só a esta pasta, em
vez de embrulhar o fonte num #pragma.

`build.flags` vale só ao compilar esta biblioteca; `build.srcFilter` garante
que apenas basic.c entre (o runtime é o nosso, em editor/src/tb_runtime.cpp).
"""

import json

PATH = "editor/lib/TinyBasic/library.json"

manifest = {
    "name": "TinyBasic",
    "version": "0.0.0-microbasic",
    "description": (
        "Stefan Lenz's IoT BASIC interpreter core, fetched and patched by "
        "patches/tinybasic/. Not vendored in this repository -- see that "
        "directory's README for why."
    ),
    "build": {
        "srcFilter": ["+<basic.c>"],
        "flags": [
            "-w",
            "-Wno-error",
        ],
    },
}

with open(PATH, "w") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")

print("library.json: OK (warnings isolados nesta biblioteca)")
