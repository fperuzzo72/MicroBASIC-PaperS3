"""Patch em cpr-vcodex/src/CrossPointSettings.h: os três campos de settings
(location/order/visible) que ShortcutDefinition{ShortcutId::MicroSlate, ...}
referencia (ver 02_patch_shortcut_registry.py) — sem eles o projeto não
compila (ponteiros-membro pra campos inexistentes).
"""

path = "cpr-vcodex/src/CrossPointSettings.h"
src = open(path).read()

# 1. location + order — logo após os de opdsBrowser.
old_loc = "  uint8_t opdsBrowserShortcut = SHORTCUT_HOME;\n  uint8_t opdsBrowserShortcutOrder = 19;"
new_loc = old_loc + "\n  uint8_t microslateShortcut = SHORTCUT_HOME;\n  uint8_t microslateShortcutOrder = 20;"
assert old_loc in src, "opdsBrowserShortcut/opdsBrowserShortcutOrder fields not found"
src = src.replace(old_loc, new_loc, 1)

# 2. visible — logo após opdsBrowserShortcutVisible.
old_vis = "  uint8_t opdsBrowserShortcutVisible = 1;"
new_vis = old_vis + "\n  uint8_t microslateShortcutVisible = 1;"
assert old_vis in src, "opdsBrowserShortcutVisible field not found"
src = src.replace(old_vis, new_vis, 1)

open(path, "w").write(src)
print("CrossPointSettings.h: OK")
