# Dual-boot: MicroBASIC and CrossPoint on the same PaperS3

The dev unit carries **two firmwares at once**, so this project and the
CrossPoint reader port can both be developed against the same physical board
without reflashing the layout every time we switch:

| slot | subtype | offset     | size   | app                                     |
|------|---------|------------|--------|-----------------------------------------|
| app0 | `ota_0` | `0x20000`  | 6656K  | CrossPoint reader (`crosspoint-reader-m5papers3`, ~5.2MB) |
| app1 | `ota_1` | `0x6A0000` | 6656K  | **MicroBASIC** (this repo, ~1.7MB)      |

The bootloader picks between them from the 8KB `otadata` partition, so
switching apps writes 32 bytes and never touches an app image.

The layout lives in [`editor/partitions.csv`](../editor/partitions.csv), and
the identical table lives in
`crosspoint-reader-m5papers3/partitions_m5papers3.csv`. **Those two files must
stay byte-identical below their comment headers** — there is one table on the
device, and each project only describes it.

```
nvs       data  nvs       0x9000     32K
otadata   data  ota       0x11000     8K
app0      app   ota_0     0x20000   6656K   <- CrossPoint
app1      app   ota_1     0x6A0000  6656K   <- MicroBASIC
coredump  data  coredump  0xD20000    64K
spiffs    data  spiffs    0xD30000  2880K   (reserved; nothing uses it today)
```

Slots are symmetric at 6656K rather than sized to today's binaries. CrossPoint
needs ~5.2MB and sets the size for both; this firmware uses ~1.7MB of its slot.
Whatever project lands in a slot next should not force a re-partition — the
whole point is that this table is written **once**. That is the change from the
previous single-app layout, which was sized to MicroBASIC alone (a 3MB `app0`)
and left no room for anything else.

`nvs` stays at 32K, for the reason in `DEVELOPMENT_LOG.md`'s "WiFi never
actually connected": 16K could not hold BLE bonds, saved WiFi credentials and
the WiFi radio's own PHY calibration blob at the same time.

## Migrating to it

`crosspoint-reader-m5papers3/docs/m5papers3-dual-boot.md` has the one-time
recipe (write the table at `0x8000`, erase `nvs` and `otadata`, then write both
apps). It is written from that repo because CrossPoint is the app that lands in
`app0` and boots by default from an erased `otadata`.

The bootloader is deliberately left alone throughout — M5Launcher's original is
still what is in flash, and is the only one ever confirmed to work here. See
`DEVELOPMENT_LOG.md`'s "The EPD rail was never powered" for why it *looked*
load-bearing for so long, and why it probably is not.

## Day to day

Build, then write only the app, only into `app1`:

```bash
pio run -e m5papers3
python3 -m esptool --chip esp32s3 --port /dev/cu.usbmodem101 --baud 921600 \
    write_flash 0x6A0000 .pio/build/m5papers3/firmware.bin
```

`python3 -m esptool`, not a bare `esptool.py`, which is not on `PATH`. If the
module is missing from your `python3`, PlatformIO's own copy is always there:
`~/.platformio/penv/bin/python -m esptool`.

`board_upload.offset_address` and `board_upload.maximum_size` in
`editor/platformio.ini` track the `app1` row, so "Checking size" measures
against the real 6656K ceiling.

**Do not use `pio run -t upload`.** It writes four images, not one:
`bootloader.bin` at `0x0`, the partition table at `0x8000`, `boot_app0.bin` at
`0x11000` — which resets `otadata` to "boot slot 0" — and the app. From here
that means flashing MicroBASIC into `app1` and then booting CrossPoint instead,
which reads as "my flash didn't take".

## Switching which app boots

```bash
editor/boot-slot.sh        # what is selected now
editor/boot-slot.sh 0      # CrossPoint
editor/boot-slot.sh 1      # MicroBASIC
```

That wraps ESP-IDF's own `otatool.py`, so the `otadata` entry (a sequence
number plus its CRC) is written by the vendor's implementation rather than a
hand-rolled one. Power-cycle with the physical button afterwards.

Both directions now work from the device itself, so the USB script is only a
fallback:

* **CrossPoint -> MicroBASIC**: its Home menu lists this firmware after
  Settings (see `crosspoint-reader-m5papers3/src/util/OtaApps.h`).
* **MicroBASIC -> CrossPoint**: the **READER** button in the status bar, behind
  a confirmation, since the cost of a stray tap is a reboot into another
  firmware. `editor/src/ota_apps.cpp`.

The reader is found rather than hardcoded: `detectOtaApps()` walks the OTA
partitions, skips the running one, and accepts a slot only if it holds a
*different* project (an `esp_app_desc_t.project_name` comparison), so an empty
slot or a stale copy of this same firmware is never offered. Its display name
comes from shared NVS, which is what `registerOtaAppName("MicroBASIC")` writes
at boot -- that is what turns "OTA Slot 1" into "MicroBASIC" in the reader's
own menu, on its next boot, with nothing to reflash on that side.

One deliberate difference from CrossPoint's copy: it writes the new otadata
entry as `ESP_OTA_IMG_NEW` and then calls `confirmLastOtaSwitch()` to rewrite
it as `VALID`, because its own path is shared with a real self-update that
*should* keep the bootloader's rollback net. Nothing here shares that path --
this only ever points at an already-flashed, previously-working sibling -- so
`ota_apps.cpp` writes `VALID` directly. Same end state, one flash write
instead of two. Left as `NEW`, the first reset before the app marks itself
valid (neither app calls `esp_ota_mark_app_valid_cancel_rollback()`) rolls
silently back to the other slot, which shows up as waking from sleep in the
app you just left.

## One consequence worth knowing

CrossPoint's own SD/OTA self-update targets "the next OTA partition", which on
this unit is *this* firmware's slot. A guard in its `FirmwareFlasher` refuses
when the target holds a different app, so self-update is disabled on the reader
while both slots are occupied. Update it over USB instead.
