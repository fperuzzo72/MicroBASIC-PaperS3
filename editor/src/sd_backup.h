#pragma once
#include <SDCardManager.h>
#include <cstdio>
#include <cstring>

// Ported from MicroBASIC's own sd_backup.h (editor/port-staging/), trimmed:
// the X4 version migrates settings off a legacy "/microslate" directory name
// from before that project existed under its current name. This device has
// no such history -- "/MicroBASIC" is the only name it has ever used -- so
// that migration step is dropped rather than carried over.
//
// "/MicroBASIC" doubles as both the settings dir (WiFi credentials today;
// BLE pairing/UI prefs if this device ever grows them) and the programs dir
// (/MicroBASIC/programs, see tb_runtime.cpp's TB_DIR).
static constexpr char SETTINGS_DIR[] = "/MicroBASIC";

// Call before anything reads/writes settings under SETTINGS_DIR. Cheap and
// idempotent -- tb_bridge.cpp's fsbegin() already creates this same
// directory at boot, so in practice this is a no-op by the time WiFi setup
// runs, but callers here shouldn't have to know that.
static inline void ensureSettingsDir() {
  if (!SdMan.exists(SETTINGS_DIR)) SdMan.mkdir(SETTINGS_DIR);
}

// Read entire file into buf. Returns false if missing or too large.
static inline bool sdReadFile(const char* path, char* buf, size_t bufSize) {
  if (!SdMan.exists(path)) return false;
  auto f = SdMan.open(path, O_RDONLY);
  if (!f.isOpen()) return false;
  int n = f.read(buf, (int)(bufSize - 1));
  f.close();
  if (n <= 0) return false;
  buf[n] = '\0';
  return true;
}

// Write string to file (overwrites). Returns false on error.
static inline bool sdWriteFile(const char* path, const char* content) {
  auto f = SdMan.open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f.isOpen()) return false;
  size_t len = strlen(content);
  size_t written = f.write((const uint8_t*)content, len);
  f.close();
  return written == len;
}

// Parse an integer value from flat JSON: {"key":42,...}
static inline int jsonGetInt(const char* json, const char* key) {
  char needle[32];
  snprintf(needle, sizeof(needle), "\"%s\":", key);
  const char* p = strstr(json, needle);
  if (!p) return -1;
  p += strlen(needle);
  while (*p == ' ') p++;
  return atoi(p);
}

// Parse a string value from flat JSON: {"key":"value",...}
// Returns true if key found (even if value is empty string).
static inline bool jsonGetStr(const char* json, const char* key, char* out, size_t outSize) {
  char needle[32];
  snprintf(needle, sizeof(needle), "\"%s\":\"", key);
  const char* p = strstr(json, needle);
  if (!p) return false;
  p += strlen(needle);
  size_t i = 0;
  while (*p && i < outSize - 1) {
    if (*p == '"') break;
    if (*p == '\\' && *(p + 1)) {
      p++;
      switch (*p) {
        case '"':  out[i++] = '"';  break;
        case '\\': out[i++] = '\\'; break;
        case 'n':  out[i++] = '\n'; break;
        default:   out[i++] = *p;   break;
      }
    } else {
      out[i++] = *p;
    }
    p++;
  }
  out[i] = '\0';
  return true;
}

// Append an escaped JSON string value (no surrounding quotes).
static inline void jsonAppendStr(char* buf, size_t bufSize, const char* src) {
  size_t pos = strlen(buf);
  while (*src && pos < bufSize - 2) {
    if (*src == '"' || *src == '\\') buf[pos++] = '\\';
    buf[pos++] = *src++;
  }
  buf[pos] = '\0';
}
