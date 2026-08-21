"""Patch em cpr-vcodex/src/util/ShortcutRegistry.h: novo ShortcutId::MicroSlate
+ entrada correspondente, usando o mesmo mecanismo de ShortcutRegistry que
todo outro atalho do CPR-vCodex já usa (não um switch/case cru em
HomeActivity.cpp, como no CrossPoint/CrossInk) — por isso este alvo tem seu
próprio conjunto de patches, em vez de reaproveitar os de crosspoint/.
"""

path = "cpr-vcodex/src/util/ShortcutRegistry.h"
src = open(path).read()

# 1. Enum ShortcutId — adiciona MicroSlate ao final.
old_enum = "  OpdsBrowser,\n};"
new_enum = "  OpdsBrowser,\n  MicroSlate,\n};"
assert old_enum in src, "ShortcutId enum closing (OpdsBrowser,\\n};) not found"
src = src.replace(old_enum, new_enum, 1)

# 2. Tamanho do array — 17 -> 18 (dois lugares: tipo de retorno e a
#    declaração static local, ambos na mesma linha do template).
old_array = "inline const std::array<ShortcutDefinition, 17>& getShortcutDefinitions() {\n  static const std::array<ShortcutDefinition, 17> definitions = {"
new_array = "inline const std::array<ShortcutDefinition, 18>& getShortcutDefinitions() {\n  static const std::array<ShortcutDefinition, 18> definitions = {"
assert old_array in src, "getShortcutDefinitions array-size declaration not found"
src = src.replace(old_array, new_array, 1)

# 3. Nova ShortcutDefinition, logo após a de OpdsBrowser.
old_opds = '''      ShortcutDefinition{ShortcutId::OpdsBrowser, StrId::STR_OPDS_BROWSER, StrId::STR_NONE_OPT, UIIcon::Library,
                         &CrossPointSettings::opdsBrowserShortcut, &CrossPointSettings::opdsBrowserShortcutOrder,
                         &CrossPointSettings::opdsBrowserShortcutVisible},'''
new_opds = old_opds + '''
      // Dual-boot switch, not an Activity — see OtaApps.h. UIIcon::Text is a
      // placeholder pending a dedicated icon.
      ShortcutDefinition{ShortcutId::MicroSlate, StrId::STR_MICROSLATE, StrId::STR_MICROSLATE_APP_DESC, UIIcon::Text,
                         &CrossPointSettings::microslateShortcut, &CrossPointSettings::microslateShortcutOrder,
                         &CrossPointSettings::microslateShortcutVisible},'''
assert old_opds in src, "OpdsBrowser ShortcutDefinition entry not found"
src = src.replace(old_opds, new_opds, 1)

open(path, "w").write(src)
print("ShortcutRegistry.h: OK")
