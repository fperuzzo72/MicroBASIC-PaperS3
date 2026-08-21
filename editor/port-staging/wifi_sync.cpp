#include "wifi_sync.h"
#include "config.h"
#include "file_manager.h"
#include "sd_backup.h"
#include "web_files_page.h"
#include "input_handler.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <SDCardManager.h>
#include <Preferences.h>
#include <esp_pm.h>
#include <esp_heap_caps.h>

// --- Internal state ---
static WebServer* server = nullptr;
static bool syncActive = false;
static SyncState syncState = SyncState::SCANNING;
static char statusText[64] = "";

extern bool screenDirty;

// --- Network list ---
static constexpr int MAX_NETWORKS = 20;
struct NetworkInfo {
  char ssid[33];
  int  rssi;
  bool encrypted;
  bool saved;  // Has stored password in NVS
};
static NetworkInfo networks[MAX_NETWORKS];
static int networkCount = 0;
static int selectedNet = 0;

// --- Password entry ---
static constexpr int MAX_PASSWORD_LEN = 63;
static char passwordBuf[MAX_PASSWORD_LEN + 1] = "";
static int  passwordLen = 0;

// --- NVS credential storage ---
static Preferences wifiPrefs;
static constexpr int MAX_SAVED_NETWORKS = 4;

static void loadSavedCredentials();
static bool getSavedPassword(const char* ssid, char* passBuf, int passBufSize);
static void saveCredential(const char* ssid, const char* pass);
static void forgetCredential(const char* ssid);

// --- Connecting state ---
static unsigned long connectStartMs = 0;
static char connectingSSID[33] = "";
static bool usedSavedPassword = false;
static bool autoConnectAttempted = false;  // True if we tried auto-connect with saved creds

// --- Sync activity tracking ---
static int filesSent = 0;       // Files downloaded by PC (GET)
static int filesReceived = 0;  // Files uploaded by PC (POST)
static int totalFilesToSync = 0; // Total .txt files on device

// --- Browser upload state (POST /upload, see startHttpServer) ---
struct NoteUploadState {
  FsFile file;
  char path[320] = "";
  size_t bytesWritten = 0;
  bool ok = false;
  bool tooLarge = false;
};
static NoteUploadState noteUpload;

static constexpr int MAX_LOG_LINES = 6;
static char syncLog[MAX_LOG_LINES][48];
static int syncLogCount = 0;

static bool pcConnected = false;

static unsigned long lastHttpActivityMs = 0;
// Idle timeout. This counts from the last HTTP request, and browsing a file
// list is mostly reading -- 60s was short enough to drop the connection while
// the user was still deciding what to download.
static constexpr unsigned long SYNC_TIMEOUT_MS = 300000;  // 5 min no HTTP
static bool syncCompletePending = false;  // Set by handler, acted on in wifiSyncLoop

// --- DONE state ---
static unsigned long doneStartMs = 0;
static constexpr unsigned long DONE_DISPLAY_MS = 3000;  // 3s before returning to menu

// --- Forward declarations ---
static void startHttpServer();
static void stopHttpServer();
static void beginScan();
static void beginConnect(const char* ssid, const char* pass);
static void enterSyncingState();
static void enterDoneState();
static void addSyncLogEntry(const char* fmt, const char* filename);

// =========================================================================
// Sync log helpers
// =========================================================================

static void resetSyncTracking() {
  filesSent = 0;
  filesReceived = 0;
  totalFilesToSync = getFileCount();
  syncLogCount = 0;
  syncCompletePending = false;
  pcConnected = false;
  for (int i = 0; i < MAX_LOG_LINES; i++) syncLog[i][0] = '\0';
}

int getSyncTotalFiles() { return totalFilesToSync; }

static void addSyncLogEntry(const char* fmt, const char* filename) {
  // Shift entries up if full
  if (syncLogCount >= MAX_LOG_LINES) {
    for (int i = 0; i < MAX_LOG_LINES - 1; i++) {
      strncpy(syncLog[i], syncLog[i + 1], sizeof(syncLog[i]) - 1);
      syncLog[i][sizeof(syncLog[i]) - 1] = '\0';
    }
    syncLogCount = MAX_LOG_LINES - 1;
  }
  snprintf(syncLog[syncLogCount], sizeof(syncLog[syncLogCount]), fmt, filename);
  syncLogCount++;
  screenDirty = true;
}

// =========================================================================
// SD card backup for WiFi credentials
// =========================================================================

static constexpr char WIFI_BACKUP_PATH[] = "/MicroBASIC/wifi.json";

static void writeWifiBackup() {
    static char buf[512];
    int count = wifiPrefs.getInt("wifi_count", 0);
    snprintf(buf, sizeof(buf), "{\"count\":%d", count);
    for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
        char sKey[16], pKey[16];
        snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", i);
        snprintf(pKey, sizeof(pKey), "wifi_pass_%d", i);
        String ssid = wifiPrefs.getString(sKey, "");
        String pass = wifiPrefs.getString(pKey, "");
        char tmp[16];
        snprintf(tmp, sizeof(tmp), ",\"s%d\":\"", i);  strncat(buf, tmp, sizeof(buf) - strlen(buf) - 1);
        jsonAppendStr(buf, sizeof(buf), ssid.c_str());  strncat(buf, "\"", sizeof(buf) - strlen(buf) - 1);
        snprintf(tmp, sizeof(tmp), ",\"p%d\":\"", i);  strncat(buf, tmp, sizeof(buf) - strlen(buf) - 1);
        jsonAppendStr(buf, sizeof(buf), pass.c_str());  strncat(buf, "\"", sizeof(buf) - strlen(buf) - 1);
    }
    strncat(buf, "}", sizeof(buf) - strlen(buf) - 1);
    ensureSettingsDir();
    sdWriteFile(WIFI_BACKUP_PATH, buf);
}

static void restoreWifiBackup() {
    static char buf[512];
    if (!sdReadFile(WIFI_BACKUP_PATH, buf, sizeof(buf))) return;
    int count = jsonGetInt(buf, "count");
    if (count <= 0) return;
    for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
        char sKey[16], pKey[16], sNvs[16], pNvs[16];
        snprintf(sKey, sizeof(sKey), "s%d", i);
        snprintf(pKey, sizeof(pKey), "p%d", i);
        snprintf(sNvs, sizeof(sNvs), "wifi_ssid_%d", i);
        snprintf(pNvs, sizeof(pNvs), "wifi_pass_%d", i);
        char ssid[64] = "", pass[128] = "";
        jsonGetStr(buf, sKey, ssid, sizeof(ssid));
        jsonGetStr(buf, pKey, pass, sizeof(pass));
        if (ssid[0]) {
            wifiPrefs.putString(sNvs, ssid);
            wifiPrefs.putString(pNvs, pass);
        }
    }
    wifiPrefs.putInt("wifi_count", count);
    DBG_PRINTF("[SYNC] Restored %d WiFi credential(s) from SD backup\n", count);
}

// =========================================================================
// NVS credential storage
// =========================================================================

static void loadSavedCredentials() {
  // Mark networks that have saved passwords
  int count = wifiPrefs.getInt("wifi_count", 0);
  for (int i = 0; i < networkCount; i++) {
    networks[i].saved = false;
    for (int j = 0; j < count && j < MAX_SAVED_NETWORKS; j++) {
      char key[16];
      snprintf(key, sizeof(key), "wifi_ssid_%d", j);
      String savedSSID = wifiPrefs.getString(key, "");
      if (savedSSID.length() > 0 && strcmp(savedSSID.c_str(), networks[i].ssid) == 0) {
        networks[i].saved = true;
        break;
      }
    }
  }
}

static bool getSavedPassword(const char* ssid, char* passBuf, int passBufSize) {
  int count = wifiPrefs.getInt("wifi_count", 0);
  for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
    char sKey[16], pKey[16];
    snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", i);
    snprintf(pKey, sizeof(pKey), "wifi_pass_%d", i);
    String savedSSID = wifiPrefs.getString(sKey, "");
    if (savedSSID.length() > 0 && strcmp(savedSSID.c_str(), ssid) == 0) {
      String savedPass = wifiPrefs.getString(pKey, "");
      strncpy(passBuf, savedPass.c_str(), passBufSize - 1);
      passBuf[passBufSize - 1] = '\0';
      return true;
    }
  }
  return false;
}

static void saveCredential(const char* ssid, const char* pass) {
  int count = wifiPrefs.getInt("wifi_count", 0);

  // Check if already saved — update in place
  for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
    char sKey[16], pKey[16];
    snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", i);
    snprintf(pKey, sizeof(pKey), "wifi_pass_%d", i);
    String savedSSID = wifiPrefs.getString(sKey, "");
    if (savedSSID.length() > 0 && strcmp(savedSSID.c_str(), ssid) == 0) {
      wifiPrefs.putString(pKey, pass);
      return;
    }
  }

  // Add new entry (wrap around if full)
  int slot = count < MAX_SAVED_NETWORKS ? count : (count % MAX_SAVED_NETWORKS);
  char sKey[16], pKey[16];
  snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", slot);
  snprintf(pKey, sizeof(pKey), "wifi_pass_%d", slot);
  wifiPrefs.putString(sKey, ssid);
  wifiPrefs.putString(pKey, pass);
  if (count < MAX_SAVED_NETWORKS) {
    wifiPrefs.putInt("wifi_count", count + 1);
  }
  writeWifiBackup();
}

static void forgetCredential(const char* ssid) {
  int count = wifiPrefs.getInt("wifi_count", 0);
  for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
    char sKey[16];
    snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", i);
    String savedSSID = wifiPrefs.getString(sKey, "");
    if (savedSSID.length() > 0 && strcmp(savedSSID.c_str(), ssid) == 0) {
      // Shift remaining entries down
      for (int j = i; j < count - 1 && j < MAX_SAVED_NETWORKS - 1; j++) {
        char srcS[16], srcP[16], dstS[16], dstP[16];
        snprintf(srcS, sizeof(srcS), "wifi_ssid_%d", j + 1);
        snprintf(srcP, sizeof(srcP), "wifi_pass_%d", j + 1);
        snprintf(dstS, sizeof(dstS), "wifi_ssid_%d", j);
        snprintf(dstP, sizeof(dstP), "wifi_pass_%d", j);
        wifiPrefs.putString(dstS, wifiPrefs.getString(srcS, ""));
        wifiPrefs.putString(dstP, wifiPrefs.getString(srcP, ""));
      }
      // Clear last slot
      int lastIdx = count - 1;
      char lastS[16], lastP[16];
      snprintf(lastS, sizeof(lastS), "wifi_ssid_%d", lastIdx);
      snprintf(lastP, sizeof(lastP), "wifi_pass_%d", lastIdx);
      wifiPrefs.remove(lastS);
      wifiPrefs.remove(lastP);
      wifiPrefs.putInt("wifi_count", count - 1);
      writeWifiBackup();
      return;
    }
  }
}

// =========================================================================
// WiFi scanning
// =========================================================================

// Automatic light sleep and 10-80MHz DFS are on for the whole firmware (see
// main.cpp), and they are not free for the radio: a scan is a timed sequence
// of channel hops, and a core that may drop to 10MHz or sleep between them
// is the wrong place to run one. This pins the clock for as long as the sync
// screen is open and hands it back on the way out, so the cost is paid only
// while the user is actually looking at a WiFi screen.
static bool pmPinned = false;

static void pinClockForRadio(bool pin) {
  if (pin == pmPinned) return;
  esp_pm_config_esp32c3_t cfg = {
    .max_freq_mhz = 80,
    .min_freq_mhz = pin ? 80 : 10,
    .light_sleep_enable = !pin,
  };
  const esp_err_t err = esp_pm_configure(&cfg);
  if (err == ESP_OK) pmPinned = pin;
  DBG_PRINTF("[SYNC] PM %s: %s\n", pin ? "pinned" : "released", esp_err_to_name(err));
}

static void beginScan() {
  syncState = SyncState::SCANNING;
  strcpy(statusText, "Scanning...");
  networkCount = 0;
  selectedNet = 0;
  // Rescanning means starting over: whatever the last connection attempt
  // used no longer describes the next one. Without this, forgetting a
  // password (which rescans) left usedSavedPassword true from the
  // auto-connect that had just failed.
  usedSavedPassword = false;
  autoConnectAttempted = false;

  pinClockForRadio(true);

  WiFi.mode(WIFI_STA);

  // DO NOT call WiFi.setSleep(false) here. WiFi's own modem power save is
  // separate from the CPU light sleep pinned above, and the IDF log made it
  // look like the obvious culprit for a slow page ("total sleep time:
  // 65986964 us / 76529859 us" -- asleep 86% of the session). Turning it off
  // aborts the firmware:
  //
  //     wifi:Set ps type: 0
  //     E wifi:Error! Should enable WiFi modem sleep when both WiFi and
  //       Bluetooth are enabled!!!!!!
  //     abort() was called at PC 0x420c849b on core 0
  //
  // On this chip WiFi and BLE share one radio, and coexistence requires the
  // WiFi side to keep sleeping so the BLE side gets the air. The keyboard is
  // BLE, and the sync screen needs it, so BLE cannot be shut down for the
  // duration either. Modem sleep stays; with the AP's DTIM period of 1 the
  // station wakes every beacon (~102ms here), which is a latency to design
  // around rather than a stall.
  WiFi.disconnect(true);

  // scanNetworks() reports failure immediately rather than through
  // scanComplete(), and the two failures look identical on screen otherwise.
  // Worth distinguishing: this device has run out of contiguous heap before
  // (BLE needed 20KB and could not get it), and bringing up the WiFi stack
  // is the other large allocation in this firmware.
  const int16_t started = WiFi.scanNetworks(true);
  DBG_PRINTF("[SYNC] scan start=%d heap=%u largest=%u\n", (int)started,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  if (started == WIFI_SCAN_FAILED) {
    syncState = SyncState::NETWORK_LIST;
    snprintf(statusText, sizeof(statusText), "Radio busy (heap %uK)",
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024));
  }

  screenDirty = true;
  DBG_PRINTLN("[SYNC] WiFi scan started");
}

static void processScanResults() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;  // Still scanning

  if (n <= 0) {
    // Scan failed or no networks found
    networkCount = 0;
    syncState = SyncState::NETWORK_LIST;
    snprintf(statusText, sizeof(statusText), "%s",
             n == 0 ? "No networks found" : "Scan failed");
    WiFi.scanDelete();
    screenDirty = true;
    return;
  }

  // Deduplicate by SSID, keeping strongest signal
  networkCount = 0;
  for (int i = 0; i < n && networkCount < MAX_NETWORKS; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;  // Skip hidden networks

    // Check for duplicate
    bool duplicate = false;
    for (int j = 0; j < networkCount; j++) {
      if (strcmp(networks[j].ssid, ssid.c_str()) == 0) {
        duplicate = true;
        if (WiFi.RSSI(i) > networks[j].rssi) {
          networks[j].rssi = WiFi.RSSI(i);
        }
        break;
      }
    }
    if (duplicate) continue;

    strncpy(networks[networkCount].ssid, ssid.c_str(), 32);
    networks[networkCount].ssid[32] = '\0';
    networks[networkCount].rssi = WiFi.RSSI(i);
    networks[networkCount].encrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    networks[networkCount].saved = false;
    networkCount++;
  }

  WiFi.scanDelete();

  // Mark saved networks
  loadSavedCredentials();

  // Sort: saved networks first, then by signal strength
  for (int i = 0; i < networkCount - 1; i++) {
    for (int j = i + 1; j < networkCount; j++) {
      bool swap = false;
      if (networks[j].saved && !networks[i].saved) {
        swap = true;
      } else if (networks[j].saved == networks[i].saved && networks[j].rssi > networks[i].rssi) {
        swap = true;
      }
      if (swap) {
        NetworkInfo tmp = networks[i];
        networks[i] = networks[j];
        networks[j] = tmp;
      }
    }
  }

  selectedNet = 0;
  syncState = SyncState::NETWORK_LIST;
  statusText[0] = '\0';
  screenDirty = true;
  DBG_PRINTF("[SYNC] Found %d networks\n", networkCount);

  // Intelligent auto-connect: if the strongest available network is saved, auto-connect
  if (networkCount > 0 && networks[0].saved) {
    char savedPass[MAX_PASSWORD_LEN + 1];
    if (getSavedPassword(networks[0].ssid, savedPass, sizeof(savedPass))) {
      usedSavedPassword = true;
      autoConnectAttempted = true;
      beginConnect(networks[0].ssid, savedPass);
      DBG_PRINTF("[SYNC] Auto-connecting to strongest saved network: %s\n", networks[0].ssid);
      return;
    }
  }
}

// =========================================================================
// Connection
// =========================================================================

static void beginConnect(const char* ssid, const char* pass) {
  strncpy(connectingSSID, ssid, 32);
  connectingSSID[32] = '\0';
  syncState = SyncState::CONNECTING;
  snprintf(statusText, sizeof(statusText), "Connecting to %s...", ssid);
  connectStartMs = millis();

  WiFi.disconnect(true);
  delay(50);
  WiFi.begin(ssid, pass);
  screenDirty = true;
  DBG_PRINTF("[SYNC] Connecting to %s\n", ssid);
}

static void enterSyncingState() {
  resetSyncTracking();
  startHttpServer();
  // Shown as a ready-to-type browser URL, not a bare IP: mDNS names
  // (microbasic.local) don't resolve on every network/OS, and the
  // numeric IP is the one address that always works regardless.
  snprintf(statusText, sizeof(statusText), "http://%s/",
           WiFi.localIP().toString().c_str());
  syncState = SyncState::SYNCING;
  lastHttpActivityMs = millis();
  screenDirty = true;
  DBG_PRINTF("[SYNC] Syncing — server at %s\n", statusText);
}

static void enterDoneState() {
  stopHttpServer();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  // Only now hand the clock back. Releasing it first meant the radio was torn
  // down with 10MHz DFS and light sleep already back on, and the teardown
  // did not always finish: "E wifi:timeout when WiFi un-init, type=4".
  pinClockForRadio(false);

  syncState = SyncState::DONE;
  doneStartMs = millis();

  if (filesSent == 0 && filesReceived == 0) {
    strcpy(statusText, "No changes");
  } else {
    snprintf(statusText, sizeof(statusText), "Sent: %d  Received: %d",
             filesSent, filesReceived);
  }
  screenDirty = true;
  DBG_PRINTF("[SYNC] Done — %s\n", statusText);
}

// The two states that put a *question* on screen. Both are entered by
// something finishing rather than by the user pressing anything, and both
// were answering themselves: the user saw "Save password?" appear and vanish
// before they could read it.
//
// Two causes, one guard. Whatever is already in the input queue was typed at
// a different screen -- the Enter that submitted the password, or a repeat of
// it -- so it is discarded on entry. And the panel takes ~700ms to actually
// show the question, so keys are ignored until it has had time to appear;
// this device also generates spurious button presses (see
// docs/DEVELOPMENT_LOG.md), and a prompt that can be dismissed before it is
// visible is exactly where that does the most damage.
static unsigned long promptOpenedMs = 0;
static constexpr unsigned long PROMPT_GUARD_MS = 900;

static void openPrompt(SyncState s) {
  inputDiscardPendingKeys();
  promptOpenedMs = millis();
  syncState = s;
  screenDirty = true;
}

static bool promptStillSettling() {
  return millis() - promptOpenedMs < PROMPT_GUARD_MS;
}

static void pollConnection() {
  if (WiFi.status() == WL_CONNECTED) {
    DBG_PRINTF("[SYNC] connected, usedSavedPassword=%d\n", (int)usedSavedPassword);
    // If we used a manually entered password, prompt to save first
    if (!usedSavedPassword) {
      snprintf(statusText, sizeof(statusText), "%s",
               WiFi.localIP().toString().c_str());
      openPrompt(SyncState::SAVE_PROMPT);
    } else {
      enterSyncingState();
    }
    return;
  }

  if (millis() - connectStartMs > 25000) {
    WiFi.disconnect(true);
    strcpy(statusText, "Connection failed");

    if (usedSavedPassword) {
      openPrompt(SyncState::FORGET_PROMPT);
    } else {
      syncState = SyncState::CONNECT_FAILED;
      screenDirty = true;
    }
    DBG_PRINTLN("[SYNC] Connection timed out");
  }
}

// =========================================================================
// HTTP Server
// =========================================================================

// --- Collections ----------------------------------------------------------
// The browser file manager serves two separate directories as tabs: the
// prose editor's notes and the BASIC programs. Everything below (list,
// download, upload, delete) is written against this descriptor rather than
// hardcoding /notes, so the two tabs share one implementation.
struct Collection {
  const char* id;        // query-string value, and the URL prefix for downloads
  const char* dir;       // where it lives on the SD card
  const char* listExt;   // only list files ending in this, or "" to list all
  const char* forceExt;  // extension forced on upload, or "" to accept as-is
  size_t maxFileSize;    // reject uploads bigger than what the device can load
};

static const Collection COLLECTIONS[] = {
    // Notes must stay filtered to .txt: the editor's save keeps one
    // generation of .bak alongside each note, and those are backups, not
    // files to offer for download or deletion.
    {"notes", "/notes", ".txt", ".txt", TEXT_BUFFER_SIZE - 1},
    // Programs are listed unfiltered and uploaded as-is: SAVE stores under
    // exactly the name typed with no forced extension (see screen_editor.h),
    // so filtering here would hide the files the user just saved.
    {"programs", "/MicroBASIC/programs", "", "", PROGRAM_UPLOAD_MAX_SIZE - 1},
};
static constexpr int COLLECTION_COUNT = sizeof(COLLECTIONS) / sizeof(COLLECTIONS[0]);

// Which collection the in-flight upload targets. Set once at
// UPLOAD_FILE_START and read by the later WRITE callbacks, which have no
// access to the request's query string.
static const Collection* uploadColl = nullptr;

// Defaults to notes when absent or unrecognised, so old clients and a bare
// /api/files keep behaving exactly as before.
static const Collection& collectionFromArg() {
  if (server->hasArg("c")) {
    String want = server->arg("c");
    for (int i = 0; i < COLLECTION_COUNT; i++) {
      if (want == COLLECTIONS[i].id) return COLLECTIONS[i];
    }
  }
  return COLLECTIONS[0];
}

static void handleFileList() {
  lastHttpActivityMs = millis();
  if (!pcConnected) {
    pcConnected = true;
    screenDirty = true;
  }

  const Collection& coll = collectionFromArg();

  auto dir = SdMan.open(coll.dir);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    // An absent directory is normal (nothing saved yet), not an error --
    // reporting 500 made the programs tab look broken on a fresh device.
    server->send(200, "application/json", "[]");
    return;
  }

  String json = "[";
  bool first = true;
  char name[256];

  const size_t extLen = strlen(coll.listExt);
  dir.rewindDirectory();
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.' || file.isDirectory()) { file.close(); continue; }

    if (extLen) {
      const size_t nameLen = strlen(name);
      if (nameLen <= extLen || strcasecmp(name + nameLen - extLen, coll.listExt) != 0) {
        file.close();
        continue;
      }
    }

    if (!first) json += ",";
    first = false;
    json += "{\"name\":\"";
    json += name;
    json += "\",\"size\":";
    json += String((unsigned long)file.size());
    json += "}";
    file.close();
  }
  dir.close();

  json += "]";
  server->send(200, "application/json", json);
}

// Serves /<collection-id>/<filename> for any collection, e.g. /notes/x.txt
// or /programs/HELLO. Returns false if the URI doesn't name a collection.
static bool handleFileDownload() {
  lastHttpActivityMs = millis();

  String uri = server->uri();
  const Collection* coll = nullptr;
  int nameStart = 0;
  for (int i = 0; i < COLLECTION_COUNT; i++) {
    String prefix = String("/") + COLLECTIONS[i].id + "/";
    if (uri.startsWith(prefix) && uri.length() > prefix.length()) {
      coll = &COLLECTIONS[i];
      nameStart = prefix.length();
      break;
    }
  }
  if (!coll) return false;

  String filename = uri.substring(nameStart);
  if (filename.indexOf("..") >= 0 || filename.indexOf('/') >= 0) {
    server->send(400, "text/plain", "Invalid name");
    return true;
  }

  char path[320];
  snprintf(path, sizeof(path), "%s/%s", coll->dir, filename.c_str());

  auto file = SdMan.open(path, O_RDONLY);
  if (!file) {
    server->send(404, "text/plain", "Not found");
    return true;
  }

  size_t fileSize = file.size();
  server->setContentLength(fileSize);
  server->send(200, "text/plain", "");

  uint8_t buf[512];
  while (file.available()) {
    int bytesRead = file.read(buf, sizeof(buf));
    if (bytesRead <= 0) break;
    server->client().write(buf, bytesRead);
  }
  file.close();

  // Track: PC downloaded a file from device = "sent"
  filesSent++;
  screenDirty = true;
  DBG_PRINTF("[SYNC] Sent file: %s\n", path);
  return true;
}

static void handleSyncComplete() {
  lastHttpActivityMs = millis();
  server->send(200, "text/plain", "OK");
  DBG_PRINTLN("[SYNC] PC signaled sync complete");
  syncCompletePending = true;  // enterDoneState() called from wifiSyncLoop, not here
}

static void handleFilesPage() {
  lastHttpActivityMs = millis();

  // Two things this deliberately does not do. It does not call send() with
  // the page as a String: that copies ~9.6KB into one contiguous allocation,
  // on a device that has had 7KB to give. And it does not hand the whole
  // 9.6KB to one write() either -- WiFiClient::write() gives up after a fixed
  // number of retries, and a body that runs out of retries is not an error
  // anyone sees, it is a page that renders and a script tag that does not
  // arrive. Sent a TCP segment at a time, each chunk gets its own retry
  // budget, so a stall costs a pause rather than the rest of the file.
  const size_t len = strlen(FILES_PAGE_HTML);
  server->setContentLength(len);
  server->send(200, "text/html", "");

  constexpr size_t CHUNK = 1440;  // one segment at the default MSS
  for (size_t off = 0; off < len; off += CHUNK) {
    const size_t n = (len - off < CHUNK) ? (len - off) : CHUNK;
    server->sendContent_P(FILES_PAGE_HTML + off, n);
  }
}

// The file manager now lives at "/" (see startHttpServer) — redirect anyone
// who still has "/files" bookmarked or typed from muscle memory.
static void handleFilesPageRedirect() {
  lastHttpActivityMs = millis();
  server->sendHeader("Location", "/");
  server->send(302, "text/plain", "");
}

// Called repeatedly with chunks of the multipart body as they arrive.
static void handleNoteUploadData() {
  HTTPUpload& upload = server->upload();
  lastHttpActivityMs = millis();

  if (upload.status == UPLOAD_FILE_START) {
    if (!pcConnected) { pcConnected = true; screenDirty = true; }

    // Which tab uploaded this. Captured here, at FILE_START, and kept for
    // the rest of the transfer: the size limit differs per collection and
    // the WRITE/END callbacks don't get to re-read the query string.
    const Collection& coll = collectionFromArg();
    uploadColl = &coll;

    // Sanitize: filename only (no path), clamp length, and force the
    // collection's extension if it has one (programs don't -- they're stored
    // under exactly the name given, matching SAVE).
    String filename = upload.filename;
    int slash = filename.lastIndexOf('/');
    if (slash >= 0) filename = filename.substring(slash + 1);
    if (filename.length() == 0) filename = String("upload") + coll.forceExt;
    if (coll.forceExt[0] && !filename.endsWith(coll.forceExt)) filename += coll.forceExt;
    if ((int)filename.length() > MAX_FILENAME_LEN - 1) filename = filename.substring(0, MAX_FILENAME_LEN - 1);

    if (strcmp(coll.id, "programs") == 0) {
      if (!SdMan.exists("/MicroBASIC")) SdMan.mkdir("/MicroBASIC");
      if (!SdMan.exists(coll.dir)) SdMan.mkdir(coll.dir);
    }

    snprintf(noteUpload.path, sizeof(noteUpload.path), "%s/%s", coll.dir, filename.c_str());
    noteUpload.file = SdMan.open(noteUpload.path, O_WRONLY | O_CREAT | O_TRUNC);
    noteUpload.bytesWritten = 0;
    noteUpload.tooLarge = false;
    noteUpload.ok = (bool)noteUpload.file;
    DBG_PRINTF("[SYNC] Upload start: %s\n", noteUpload.path);

  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!noteUpload.ok) return;
    // Reject before it becomes another instance of the TEXT_BUFFER_SIZE
    // silent-truncation trap (see config.h) — a note that can't fully load
    // into the editor shouldn't be accepted in the first place. Programs are
    // bounded by the interpreter's memory instead, not by any buffer here.
    if (noteUpload.bytesWritten + upload.currentSize >
        (uploadColl ? uploadColl->maxFileSize : TEXT_BUFFER_SIZE - 1)) {
      noteUpload.ok = false;
      noteUpload.tooLarge = true;
      noteUpload.file.close();
      SdMan.remove(noteUpload.path);
      return;
    }
    size_t written = noteUpload.file.write(upload.buf, upload.currentSize);
    noteUpload.bytesWritten += written;
    if (written != upload.currentSize) noteUpload.ok = false;

  } else if (upload.status == UPLOAD_FILE_END) {
    if (noteUpload.file) noteUpload.file.close();
    if (noteUpload.ok) {
      filesReceived++;
      screenDirty = true;
      DBG_PRINTF("[SYNC] Upload complete: %s (%u bytes)\n", noteUpload.path,
                 (unsigned)noteUpload.bytesWritten);
    }

  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (noteUpload.file) noteUpload.file.close();
    noteUpload.ok = false;
  }
}

// Called once after the whole request body has been consumed — sends the response.
static void handleNoteUploadDone() {
  lastHttpActivityMs = millis();
  const size_t limit = uploadColl ? uploadColl->maxFileSize : TEXT_BUFFER_SIZE - 1;
  if (noteUpload.ok) {
    // Only the notes list is mirrored in the device's own file browser.
    if (!uploadColl || strcmp(uploadColl->id, "notes") == 0) refreshFileList();
    server->send(200, "text/plain", "OK");
  } else if (noteUpload.tooLarge) {
    char msg[64];
    snprintf(msg, sizeof(msg), "File too large (%uKB max)", (unsigned)((limit + 1) / 1024));
    server->send(400, "text/plain", msg);
  } else {
    server->send(400, "text/plain", "Upload failed");
  }
  uploadColl = nullptr;
}

static void handleDeleteNote() {
  lastHttpActivityMs = millis();
  if (!server->hasArg("name")) {
    server->send(400, "text/plain", "Missing name");
    return;
  }
  const Collection& coll = collectionFromArg();
  String name = server->arg("name");
  // No path traversal or targeting anything outside the collection's dir.
  if (name.length() == 0 || name.indexOf('/') >= 0 || name.indexOf("..") >= 0) {
    server->send(400, "text/plain", "Invalid name");
    return;
  }
  char path[320];
  snprintf(path, sizeof(path), "%s/%s", coll.dir, name.c_str());
  if (!SdMan.exists(path)) {
    server->send(404, "text/plain", "Not found");
    return;
  }
  if (strcmp(coll.id, "notes") == 0) {
    deleteFile(name.c_str());  // also removes the .bak and refreshes the list
  } else {
    // Programs have no .bak generation and aren't in the device's file
    // browser, so a plain remove is the whole job.
    SdMan.remove(path);
  }
  server->send(200, "text/plain", "OK");
  screenDirty = true;
  DBG_PRINTF("[SYNC] Deleted via browser: %s\n", path);
}

static void handleNotFound() {
  if (server->method() == HTTP_GET && handleFileDownload()) return;
  server->send(404, "text/plain", "Not found");
}

static void startHttpServer() {
  if (server) return;
  server = new WebServer(80);
  server->on("/api/files", HTTP_GET, handleFileList);
  server->on("/api/sync-complete", HTTP_POST, handleSyncComplete);
  server->on("/", HTTP_GET, handleFilesPage);
  server->on("/files", HTTP_GET, handleFilesPageRedirect);
  server->on("/upload", HTTP_POST, handleNoteUploadDone, handleNoteUploadData);
  server->on("/delete", HTTP_POST, handleDeleteNote);
  server->onNotFound(handleNotFound);
  server->begin();
  MDNS.begin("microbasic");
  DBG_PRINTF("[SYNC] HTTP server at %s heap=%u largest=%u\n",
             WiFi.localIP().toString().c_str(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void stopHttpServer() {
  if (server) {
    server->stop();
    delete server;
    server = nullptr;
  }
  MDNS.end();
}

// =========================================================================
// Input handling — called from input_handler for all key events
// =========================================================================

void syncHandleKey(uint8_t keyCode, uint8_t modifiers) {
  switch (syncState) {
    case SyncState::SCANNING:
      // No input during scan
      if (keyCode == HID_KEY_ESCAPE) {
        wifiSyncStop();
      }
      break;

    case SyncState::NETWORK_LIST:
      if (keyCode == HID_KEY_DOWN && networkCount > 0) {
        selectedNet = (selectedNet + 1) % networkCount;
        screenDirty = true;
      } else if (keyCode == HID_KEY_UP && networkCount > 0) {
        selectedNet = (selectedNet - 1 + networkCount) % networkCount;
        screenDirty = true;
      } else if (keyCode == HID_KEY_ENTER && networkCount > 0) {
        DBG_PRINTF("[SYNC] picked %s\n", networks[selectedNet].ssid);
        // Try saved password first
        char savedPass[MAX_PASSWORD_LEN + 1];
        if (getSavedPassword(networks[selectedNet].ssid, savedPass, sizeof(savedPass))) {
          usedSavedPassword = true;
          autoConnectAttempted = false;
          beginConnect(networks[selectedNet].ssid, savedPass);
        } else if (!networks[selectedNet].encrypted) {
          // Open network — connect directly
          usedSavedPassword = false;
          autoConnectAttempted = false;
          beginConnect(networks[selectedNet].ssid, "");
        } else {
          // Need password
          usedSavedPassword = false;
          autoConnectAttempted = false;
          passwordBuf[0] = '\0';
          passwordLen = 0;
          syncState = SyncState::PASSWORD_ENTRY;
          screenDirty = true;
        }
      } else if (keyCode == HID_KEY_ESCAPE) {
        wifiSyncStop();
      }
      break;

    case SyncState::PASSWORD_ENTRY:
      if (keyCode == HID_KEY_ENTER) {
        if (passwordLen > 0) {
          beginConnect(networks[selectedNet].ssid, passwordBuf);
        }
      } else if (keyCode == HID_KEY_ESCAPE) {
        syncState = SyncState::NETWORK_LIST;
        screenDirty = true;
      } else if (keyCode == HID_KEY_BACKSPACE) {
        if (passwordLen > 0) {
          passwordLen--;
          passwordBuf[passwordLen] = '\0';
          screenDirty = true;
        }
      } else {
        // Printable character — reuse hidToAscii from input_handler
        extern char hidToAscii(uint8_t hid, uint8_t modifiers);
        char c = hidToAscii(keyCode, modifiers);
        if (c != 0 && c >= ' ' && c != '\n' && c != '\t' && passwordLen < MAX_PASSWORD_LEN) {
          passwordBuf[passwordLen++] = c;
          passwordBuf[passwordLen] = '\0';
          screenDirty = true;
        }
      }
      break;

    case SyncState::CONNECTING:
      // No input while connecting (timeout handles failure)
      if (keyCode == HID_KEY_ESCAPE) {
        WiFi.disconnect(true);
        if (autoConnectAttempted) {
          // Was auto-connecting — fall back to scan
          beginScan();
        } else {
          syncState = SyncState::NETWORK_LIST;
          screenDirty = true;
        }
      }
      break;

    case SyncState::SYNCING:
      if (keyCode == HID_KEY_ESCAPE) {
        wifiSyncStop();
      }
      break;

    case SyncState::DONE:
      // Any key press returns to menu immediately
      wifiSyncStop();
      break;

    case SyncState::CONNECT_FAILED:
      if (keyCode == HID_KEY_ENTER) {
        // Back to network list, re-scan
        beginScan();
      } else if (keyCode == HID_KEY_ESCAPE) {
        wifiSyncStop();
      }
      break;

    case SyncState::SAVE_PROMPT:
      DBG_PRINTF("[SYNC] SAVE_PROMPT key=%02x settling=%d\n",
                 (unsigned)keyCode, (int)promptStillSettling());
      if (promptStillSettling()) break;
      // Up = Yes (save), Down = No (skip)
      if (keyCode == HID_KEY_UP || keyCode == HID_KEY_ENTER) {
        saveCredential(connectingSSID, passwordBuf);
        DBG_PRINTF("[SYNC] Saved credentials for %s\n", connectingSSID);
        enterSyncingState();
      } else if (keyCode == HID_KEY_DOWN || keyCode == HID_KEY_ESCAPE) {
        enterSyncingState();
      }
      break;

    case SyncState::FORGET_PROMPT:
      if (promptStillSettling()) break;
      // Up = Yes (forget), Down = No (keep)
      if (keyCode == HID_KEY_UP || keyCode == HID_KEY_ENTER) {
        forgetCredential(connectingSSID);
        DBG_PRINTF("[SYNC] Forgot credentials for %s\n", connectingSSID);
        beginScan();
      } else if (keyCode == HID_KEY_DOWN || keyCode == HID_KEY_ESCAPE) {
        // Keep credentials — go to network list so user can retry manually
        // rather than triggering auto-connect again immediately
        syncState = SyncState::NETWORK_LIST;
        screenDirty = true;
      }
      break;
  }
}

// =========================================================================
// Public API
// =========================================================================

void wifiSyncStart() {
  if (syncActive) return;
  syncActive = true;
  // These are statics that outlive a sync session, and a session can end in
  // any state -- including one that set them. A stale `usedSavedPassword`
  // suppresses the "save password?" prompt on the next manual connect, which
  // is exactly the class of bug being chased here.
  usedSavedPassword = false;
  autoConnectAttempted = false;
  wifiPrefs.begin("wifi_creds", false);
  if (!wifiPrefs.isKey("wifi_count")) restoreWifiBackup();
  resetSyncTracking();

  beginScan();

  DBG_PRINTLN("[SYNC] WiFi sync started");
}

void wifiSyncStop() {
  if (!syncActive) return;

  stopHttpServer();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  wifiPrefs.end();
  syncActive = false;
  networkCount = 0;
  passwordBuf[0] = '\0';
  passwordLen = 0;
  statusText[0] = '\0';

  // Return to main menu
  extern UIState currentState;
  currentState = UIState::MAIN_MENU;
  screenDirty = true;

  DBG_PRINTLN("[SYNC] WiFi sync stopped");
}

void wifiSyncLoop() {
  if (!syncActive) return;

  switch (syncState) {
    case SyncState::SCANNING:
      processScanResults();
      break;

    case SyncState::CONNECTING:
      pollConnection();
      break;

    case SyncState::SYNCING:
      if (server) server->handleClient();
      if (syncCompletePending) {
        syncCompletePending = false;
        enterDoneState();
      } else if (millis() - lastHttpActivityMs > SYNC_TIMEOUT_MS) {
        DBG_PRINTLN("[SYNC] Timeout — no HTTP activity for 60s");
        enterDoneState();
      }
      break;

    case SyncState::DONE:
      // Auto-return to menu after 3 seconds
      if (millis() - doneStartMs > DONE_DISPLAY_MS) {
        wifiSyncStop();
      }
      break;

    case SyncState::SAVE_PROMPT:
      // Server is NOT running during save prompt (will start after user responds)
      break;

    default:
      break;
  }
}

bool isWifiSyncActive() {
  return syncActive;
}

SyncState getSyncState() {
  return syncState;
}

int getNetworkCount() {
  return networkCount;
}

const char* getNetworkSSID(int i) {
  if (i < 0 || i >= networkCount) return "";
  return networks[i].ssid;
}

int getNetworkRSSI(int i) {
  if (i < 0 || i >= networkCount) return -100;
  return networks[i].rssi;
}

bool isNetworkEncrypted(int i) {
  if (i < 0 || i >= networkCount) return false;
  return networks[i].encrypted;
}

bool isNetworkSaved(int i) {
  if (i < 0 || i >= networkCount) return false;
  return networks[i].saved;
}

int getSelectedNetwork() {
  return selectedNet;
}

const char* getPasswordBuffer() {
  return passwordBuf;
}

int getPasswordLen() {
  return passwordLen;
}

const char* getSyncStatusText() {
  return statusText;
}

int getSyncFilesSent() {
  return filesSent;
}

int getSyncFilesReceived() {
  return filesReceived;
}

bool isPcConnected() {
  return pcConnected;
}
