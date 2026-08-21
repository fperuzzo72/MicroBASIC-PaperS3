# CPR-vCodex patch set (MicroBASIC variant)

Adds dual-boot support to a [CPR-vCodex](https://github.com/franssjz/cpr-vcodex)
checkout: a Home-menu shortcut that switches to the editor slot, and a
self-update guard that keeps CPR-vCodex's own firmware-update feature
from overwriting it.

This is a copy of MicroWriter's own `patches/cpr-vcodex/` set, not an
independent implementation — see that repo for the full design writeup.
The two only differ in one place: `07_patch_i18n_strings.py`'s string
*value* (`STR_MICROSLATE: "MicroBASIC"` here, `"MicroWriter"` there).
Every internal identifier (`ShortcutId::MicroSlate`, `microslateShortcut`,
`switchToFirstOtaApp`, the `STR_MICROSLATE` key name itself, ...) is left
exactly as-is — those never reach the user, and keeping them identical
means a future upstream patch update from MicroWriter's set can still be
diffed/ported cleanly.

**Verified against:** CPR-vCodex `1.5.0.9-cpr-vcodex`, same commit
MicroWriter's own set targets.

## Usage

```bash
git clone --branch 1.5.0.9-cpr-vcodex https://github.com/franssjz/cpr-vcodex.git cpr-vcodex
cd cpr-vcodex && git submodule update --init --recursive && cd ..
for f in patches/cpr-vcodex/*.py; do python3 "$f"; done
cd cpr-vcodex
pio run -e gh_release   # or default / slim — see cpr-vcodex/platformio.ini
```

Then slot-flash `cpr-vcodex/.pio/build/<env>/firmware.bin` to the reader
partition (`app0`, offset `0x10000` on this project's partition table) —
never the merged/full image, to avoid wiping NVS (BLE pairing, WiFi
credentials) or otadata.
