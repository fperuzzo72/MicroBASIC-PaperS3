#pragma once
#include <SDCardManager.h>
#include <cstdio>
#include <cstring>

// --- Settings/backup directory ---------------------------------------
// MicroBASIC's own SD folder ("/MicroBASIC") doubles as both the settings
// dir (BLE pairing, WiFi credentials, UI prefs) and the programs dir
// (/MicroBASIC/programs, see program_store.h) -- deliberately its own
// name, distinct from MicroWriter's own "/microwriter", so both firmwares
// can share one SD card without their settings colliding, even though
// MicroBASIC's editor/ is a superset of MicroWriter's (see
// docs/DEVELOPMENT_LOG.md). Was "/microslate" before this project ever
// forked from MicroWriter -- MicroWriter renamed its own copy of this to
// "/microwriter" (commit c0d6977) but that fix predates the fork point
// here, so this project still needs its own migration off the original
// "/microslate" name, straight to "/MicroBASIC" rather than through
// "/microwriter" as an intermediate step.
static constexpr char SETTINGS_DIR[] = "/MicroBASIC";
static constexpr char SETTINGS_DIR_LEGACY[] = "/microslate";

// Call once at boot, before anything reads/writes BLE pairing / WiFi
// credentials / UI prefs. Unlike MicroWriter's version of this idea, this
// can't just rename the whole legacy directory onto the new one --
// SETTINGS_DIR may already exist for an unrelated reason (BASIC programs
// live under it too) -- so this migrates the known settings files
// individually instead, leaving programs/ (and anything else already
// under SETTINGS_DIR) untouched.
static inline void ensureSettingsDir() {
    if (!SdMan.exists(SETTINGS_DIR)) SdMan.mkdir(SETTINGS_DIR);
    if (!SdMan.exists(SETTINGS_DIR_LEGACY)) return;
    static const char* kLegacyFiles[] = {"ble_kb.json", "wifi.json", "ui_prefs.json"};
    char oldPath[64];
    char newPath[64];
    for (const char* name : kLegacyFiles) {
        snprintf(oldPath, sizeof(oldPath), "%s/%s", SETTINGS_DIR_LEGACY, name);
        snprintf(newPath, sizeof(newPath), "%s/%s", SETTINGS_DIR, name);
        if (SdMan.exists(oldPath) && !SdMan.exists(newPath)) {
            SdMan.rename(oldPath, newPath);
        }
    }
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
