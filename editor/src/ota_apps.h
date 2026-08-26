#pragma once

// Dual-boot sibling apps -- the return trip to the CrossPoint reader.
//
// This unit carries two unrelated firmwares at once, CrossPoint in app0 and
// MicroBASIC in app1, and the bootloader picks between them from otadata (see
// docs/DUAL_BOOT.md). CrossPoint's Home menu can already switch *here*; this
// is the other direction, behind the status bar's READER button.
//
// The register/detect/switch scheme is ported from CrossPoint's own
// src/util/OtaApps.h, which this repo also generates into it via
// patches/cpr-vcodex/01_create_otaapps_h.py. Identifier names are kept
// deliberately identical across all of those so the copies stay diffable --
// so `registerOtaAppName`, not this project's usual camelCase-with-a-verb
// house style.
//
// Two things CrossPoint gets from elsewhere are inlined here instead, because
// this firmware has neither:
//
//   * ota_boot::switchTo(), the raw otadata write. CrossPoint has it as
//     src/network/OtaBootSwitch.cpp for its own SD/OTA self-update. It exists
//     at all because esp_ota_set_boot_partition() fails on this silicon with a
//     bogus efuse-blk-rev verification error, so the otadata entry is written
//     by hand -- the same scheme the project's web flasher uses.
//   * the "is this slot a different project" test, which CrossPoint takes from
//     its FirmwareFlasher. Here it is a plain esp_app_desc_t project_name
//     comparison, which is all that test needs to be.

#include <esp_partition.h>

#include <cstdint>

constexpr int MAX_OTA_APPS = 4;

struct OtaAppEntry {
  char name[32];
  int partitionSubtype;
};

// Records this app's display name in shared NVS, keyed by the OTA slot it is
// running from, so the sibling can list it by name instead of by slot number.
// Call once at boot: until this has run at least once, CrossPoint's Home menu
// shows this firmware as "OTA Slot 1".
void registerOtaAppName(const char* name);

// Fills `apps[]` with the sibling apps in the other OTA slots and returns how
// many were found. A slot counts only if it holds a *different* project than
// the one running, so an empty slot -- or a stale copy of this same firmware
// -- is never offered as somewhere to switch to.
int detectOtaApps(OtaAppEntry* apps, int maxApps);

// Points otadata at `partitionSubtype` and restarts into it. Does nothing if
// that partition does not exist or the otadata write fails, so a failed switch
// leaves the device running this firmware rather than unbootable.
void switchToOtaApp(int partitionSubtype);
