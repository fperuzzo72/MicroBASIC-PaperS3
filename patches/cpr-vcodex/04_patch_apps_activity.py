"""Patch em cpr-vcodex/src/activities/apps/AppsActivity.cpp: dispara o
switch de dual-boot quando o atalho MicroSlate é ativado a partir da tela
de Apps.
"""

path = "cpr-vcodex/src/activities/apps/AppsActivity.cpp"
src = open(path).read()

old = '''    case ShortcutId::OpdsBrowser:
      activityManager.goToBrowser();
      return;
  }'''
new = '''    case ShortcutId::OpdsBrowser:
      activityManager.goToBrowser();
      return;
    case ShortcutId::MicroSlate:
      switchToFirstOtaApp();
      return;
  }'''
assert old in src, "OpdsBrowser case + switch closing brace not found"
src = src.replace(old, new, 1)

# Include OtaApps.h — não existia antes deste patch. AppsActivity.cpp não
# inclui util/ShortcutRegistry.h diretamente (vem via AppsActivity.h), então
# ancoramos no próprio include local de topo.
old_include = '#include "AppsActivity.h"'
new_include = old_include + '\n#include "util/OtaApps.h"'
assert old_include in src, "AppsActivity.h include not found"
src = src.replace(old_include, new_include, 1)

open(path, "w").write(src)
print("AppsActivity.cpp: OK")
