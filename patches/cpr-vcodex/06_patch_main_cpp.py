"""Patch em cpr-vcodex/src/main.cpp: include + registerOtaAppName("CPR-vCodex")."""

path = "cpr-vcodex/src/main.cpp"
src = open(path).read()

old1 = '#include "util/ScreenshotUtil.h"'
new1 = '#include "util/ScreenshotUtil.h"\n#include "util/OtaApps.h"'
assert old1 in src, "util/ScreenshotUtil.h include not found"
src = src.replace(old1, new1, 1)

old2 = "  gpio.begin();\n  powerManager.begin();"
new2 = '  gpio.begin();\n  registerOtaAppName("CPR-vCodex");\n  powerManager.begin();'
assert old2 in src, "gpio.begin()/powerManager.begin() sequence not found"
src = src.replace(old2, new2, 1)

open(path, "w").write(src)
print("main.cpp: OK")
