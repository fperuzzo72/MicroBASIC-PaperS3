"""Patch em cpr-vcodex/src/activities/home/HomeActivity.cpp: dispara o
switch de dual-boot quando o atalho MicroSlate está fixado direto na Home
(em vez de dentro da tela de Apps — ver 04_patch_apps_activity.py para
esse outro caminho).
"""

path = "cpr-vcodex/src/activities/home/HomeActivity.cpp"
src = open(path).read()

old = '''        case ShortcutId::OpdsBrowser:
          onOpdsBrowserOpen();
          break;
      }'''
new = '''        case ShortcutId::OpdsBrowser:
          onOpdsBrowserOpen();
          break;
        case ShortcutId::MicroSlate:
          switchToFirstOtaApp();
          break;
      }'''
assert old in src, "OpdsBrowser case + switch closing brace not found"
src = src.replace(old, new, 1)

old_include = '#include "HomeActivity.h"'
new_include = old_include + '\n#include "util/OtaApps.h"'
assert old_include in src, "HomeActivity.h include not found"
src = src.replace(old_include, new_include, 1)

open(path, "w").write(src)
print("HomeActivity.cpp: OK")
