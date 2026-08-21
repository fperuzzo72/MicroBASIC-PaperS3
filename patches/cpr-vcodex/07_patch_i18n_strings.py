"""Patch em cpr-vcodex/lib/I18n/translations/english.yaml: as duas strings
que ShortcutDefinition{ShortcutId::MicroSlate, StrId::STR_MICROSLATE,
StrId::STR_MICROSLATE_APP_DESC, ...} referencia (ver
02_patch_shortcut_registry.py). O StrId enum é gerado a partir dessas
chaves em tempo de build (scripts/gen_i18n.py) — chaves ausentes em
outros idiomas caem de volta pro inglês automaticamente, então só
precisamos adicionar aqui.

Único ponto onde este conjunto de patches difere do de MicroWriter
(patches/cpr-vcodex/07_patch_i18n_strings.py lá): o VALOR das strings,
não as chaves (StrId::STR_MICROSLATE continua se chamando assim nos dois
— é só um identificador interno, nunca aparece pro usuário). O texto
exibido aqui é "MicroBASIC", já que é esse o editor que este slot
realmente contém quando build+flash é feito a partir deste repositório.
"""

path = "cpr-vcodex/lib/I18n/translations/english.yaml"
src = open(path).read()

old = 'STR_OPDS_BROWSER: "OPDS Browser"'
new = old + '\nSTR_MICROSLATE: "MicroBASIC"\nSTR_MICROSLATE_APP_DESC: "Switch to the BASIC environment"'
assert old in src, "STR_OPDS_BROWSER key not found"
src = src.replace(old, new, 1)

open(path, "w").write(src)
print("english.yaml: OK")
