#include "sd_datetime.h"

#include <Arduino.h>
#include <Rtc.h>
#include <SdFat.h>

#include <ctime>

namespace {

freeink::Rtc rtc;
bool rtcReady = false;

// The firmware's own build date, as the fallback. Parsed from __DATE__, which
// is the C standard's "Mmm dd yyyy".
struct BuildDate {
  uint16_t year;
  uint8_t month;
  uint8_t day;
};

BuildDate buildDate() {
  static const char kMonths[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* d = __DATE__;

  BuildDate out{2026, 1, 1};
  for (int i = 0; i < 12; ++i) {
    if (strncmp(d, kMonths + i * 3, 3) == 0) {
      out.month = (uint8_t)(i + 1);
      break;
    }
  }
  out.day = (uint8_t)atoi(d + 4);
  out.year = (uint16_t)atoi(d + 7);
  if (out.day < 1 || out.day > 31) out.day = 1;
  return out;
}

// FAT cannot represent anything before 1980, and SdFat's FS_DATE would wrap a
// smaller year into nonsense rather than refuse it.
bool fatRepresentable(uint16_t year) { return year >= 1980 && year <= 2107; }

// Rtc::now() returning true only means the oscillator is running, not that
// anyone ever set it. A never-set BM8563 on this unit reads back 2077, which
// FAT is perfectly happy to store -- a wrong date that looks right is worse
// than no date, because nothing downstream can tell it is wrong.
//
// The firmware cannot legitimately be reading a moment before it was built,
// so the build date is a hard floor. The ceiling is generous but finite: a
// clock decades ahead is garbage, not longevity, and if this device really is
// still running then, one SYNC re-sets it.
bool plausible(const freeink::Rtc::DateTime& dt) {
  const BuildDate b = buildDate();
  if (dt.year < b.year) return false;
  if (dt.year > (uint16_t)(b.year + 30)) return false;
  if (dt.year == b.year && dt.month < b.month) return false;
  return dt.month >= 1 && dt.month <= 12 && dt.day >= 1 && dt.day <= 31;
}

void dateTimeCallback(uint16_t* date, uint16_t* time, uint8_t* ms10) {
  freeink::Rtc::DateTime now;
  if (rtcReady && rtc.now(now) && plausible(now) && fatRepresentable(now.year)) {
    *date = FS_DATE(now.year, now.month, now.day);
    *time = FS_TIME(now.hour, now.minute, now.second);
    *ms10 = 0;
    return;
  }

  // No trustworthy clock: the build date, at midnight. Every file written
  // before the first network sync shares it, which is honest -- they were all
  // written by this firmware, and nothing here knows when.
  const BuildDate b = buildDate();
  *date = FS_DATE(fatRepresentable(b.year) ? b.year : 1980, b.month, b.day);
  *time = FS_TIME(0, 0, 0);
  *ms10 = 0;
}

}  // namespace

void sdDateTimeSetup() {
  rtcReady = rtc.begin();

  freeink::Rtc::DateTime now;
  const bool running = rtcReady && rtc.now(now);
  if (running && !plausible(now)) {
    Serial.printf("[time] RTC reads %04u-%02u-%02u, which is not a time this firmware could be running at"
                  " -- treating it as never set\n",
                  now.year, now.month, now.day);
  }
  if (running && plausible(now)) {
    Serial.printf("[time] RTC %04u-%02u-%02u %02u:%02u:%02u UTC\n", now.year, now.month, now.day, now.hour, now.minute,
                  now.second);
  } else {
    const BuildDate b = buildDate();
    Serial.printf("[time] no RTC time yet; files dated %04u-%02u-%02u (build date) until SYNC sets it\n", b.year,
                  b.month, b.day);
  }

  FsDateTime::setCallback(dateTimeCallback);
}

bool sdDateTimeHasClock() {
  freeink::Rtc::DateTime now;
  return rtcReady && rtc.now(now) && plausible(now);
}

bool sdDateTimeSyncFromNetwork(uint32_t timeoutMs) {
  if (!rtcReady) return false;

  // UTC: offset 0, no DST rule. See the header for why.
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  // configTime() is asynchronous, so wait for the clock to actually move past
  // the epoch rather than assuming it landed.
  const uint32_t start = millis();
  time_t nowSecs = 0;
  while (millis() - start < timeoutMs) {
    nowSecs = ::time(nullptr);
    if (nowSecs > 1600000000) break;  // ~2020, i.e. a real answer
    delay(100);
  }
  if (nowSecs <= 1600000000) {
    Serial.println("[time] SNTP did not answer; keeping the previous clock");
    return false;
  }

  struct tm utc;
  gmtime_r(&nowSecs, &utc);

  freeink::Rtc::DateTime dt;
  dt.year = (uint16_t)(utc.tm_year + 1900);
  dt.month = (uint8_t)(utc.tm_mon + 1);
  dt.day = (uint8_t)utc.tm_mday;
  dt.hour = (uint8_t)utc.tm_hour;
  dt.minute = (uint8_t)utc.tm_min;
  dt.second = (uint8_t)utc.tm_sec;
  dt.weekday = (uint8_t)utc.tm_wday;

  if (!rtc.set(dt)) {
    Serial.println("[time] SNTP answered but the RTC would not take it");
    return false;
  }

  Serial.printf("[time] RTC set from network: %04u-%02u-%02u %02u:%02u:%02u UTC\n", dt.year, dt.month, dt.day, dt.hour,
                dt.minute, dt.second);
  return true;
}
