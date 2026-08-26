#pragma once

#include <cstdint>

// Dates on the files this device writes to the SD card.
//
// SdFat only fills a directory entry's date fields if a callback is
// registered:
//
//     if (FsDateTime::callback) {
//       FsDateTime::callback(&date, &time, &ms10);
//       setLe16(dir->modifyDate, date);  ...
//     }
//
// Without one the fields stay zero, and FAT reads zero as 1980-00-00 -- which
// is how this firmware's files looked when the card was opened on a computer.
//
// Where the time comes from
// -------------------------
// This board has a real RTC (BM8563, PCF8563-compatible -- see
// freeink-sdk/docs/m5papers3-support.md), so unlike the X4 there is a true
// clock to read. It is not trusted just because it answers: Rtc::now()
// returning true only means the oscillator is running, and a never-set unit
// reads back a running but meaningless date (2077 on the dev board). The time
// is used only if it is one this firmware could plausibly be running at --
// see plausible() in the .cpp. A wrong date that looks right is worse than no
// date, because nothing downstream can tell.
//
// The RTC is set from the network the first time SYNC connects to WiFi
// (sdDateTimeSyncFromNetwork()). Nothing else on this device knows the time,
// so until that has happened once, files fall back to the firmware's build
// date: not the real time, but plausible, sortable, and never 1980.
//
// Everything is UTC, deliberately. A file timestamp is a reference, not a
// calendar appointment, and UTC avoids carrying a timezone setting and DST
// rules for something nothing here reads back.

// Registers the SdFat callback. Call once at boot, before anything writes to
// the card.
void sdDateTimeSetup();

// True once there is a real clock to read: the RTC is present and running.
bool sdDateTimeHasClock();

// Asks the network for the time (SNTP) and stores it in the RTC. Call while
// WiFi is connected; blocks for up to `timeoutMs`. Returns true if the RTC was
// set. A failure is harmless -- the build-date fallback stays in place.
bool sdDateTimeSyncFromNetwork(uint32_t timeoutMs = 5000);
