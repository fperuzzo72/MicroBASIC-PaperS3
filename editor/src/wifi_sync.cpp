// Ported from MicroBASIC's own wifi_sync.cpp (editor/port-staging/) --
// the WiFi-connect wizard and the HTTP file-transfer server, minus two
// things deliberately left behind:
//
// - The MicroSlate-inherited "sync" handshake. That project's own PC-side
//   companion (editor/sync/microslate_sync.py on the X4 side, not carried
//   into this repo at all) polls /api/files, downloads new notes, and
//   POSTs /api/sync-complete when done so the device knows to shut WiFi
//   off. This port has no such companion -- the browser page never calls
//   that endpoint either -- so the route, its handler, and the
//   syncCompletePending flag are dropped outright. What's left (the
//   5-minute HTTP-idle timeout in wifiSyncLoop()'s SYNCING case) is what
//   actually ends a browser-only session.
// - The X4's own CPU-clock-pinning-during-scan workaround
//   (esp_pm_configure() around WiFi.scanNetworks()) -- specific to a
//   power-management issue on that device's ESP32-C3, not carried over.
//
// Everything else -- the network scan/connect state machine, NVS +
// SD-backup credential storage, and the four real file-transfer endpoints
// -- is the same logic, adapted to this port's single file collection
// (BASIC programs; see web_files_page.h's own comment) and its own
// input/logging conventions.

#include "wifi_sync.h"
#include "config.h"
#include "sd_backup.h"
#include "web_files_page.h"
#include "input_handler.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <SDCardManager.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <BleKeyboardHost.h>

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

// --- Transfer activity tracking (ordinary HTTP activity, not MicroSlate-specific) ---
static int filesSent = 0;      // Files downloaded by a browser (GET)
static int filesReceived = 0;  // Files uploaded by a browser (POST)

// --- Browser upload state (POST /upload, see startHttpServer) ---
struct UploadState {
  FsFile file;
  char path[320] = "";
  size_t bytesWritten = 0;
  bool ok = false;
  bool tooLarge = false;
};
static UploadState upload_;

static bool pcConnected = false;

static unsigned long lastHttpActivityMs = 0;
// Idle timeout, counted from the last HTTP request. Browsing a file list is
// mostly reading, so this needs to be generous enough that deciding what to
// download doesn't drop the connection out from under the user.
static constexpr unsigned long SYNC_TIMEOUT_MS = 300000;  // 5 min no HTTP

// --- DONE state ---
static unsigned long doneStartMs = 0;
static constexpr unsigned long DONE_DISPLAY_MS = 3000;  // 3s before returning to the editor

// --- Forward declarations ---
static void startHttpServer();
static void stopHttpServer();
static void beginScan();
static void beginConnect(const char* ssid, const char* pass);
static void enterSyncingState();
static void enterDoneState();

static void resetTransferTracking() {
  filesSent = 0;
  filesReceived = 0;
  pcConnected = false;
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
  Serial.printf("[wifi] restored %d credential(s) from SD backup\n", count);
}

// =========================================================================
// NVS credential storage
// =========================================================================

static void loadSavedCredentials() {
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

  for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
    char sKey[16], pKey[16];
    snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", i);
    snprintf(pKey, sizeof(pKey), "wifi_pass_%d", i);
    String savedSSID = wifiPrefs.getString(sKey, "");
    if (savedSSID.length() > 0 && strcmp(savedSSID.c_str(), ssid) == 0) {
      wifiPrefs.putString(pKey, pass);
      writeWifiBackup();
      return;
    }
  }

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
      for (int j = i; j < count - 1 && j < MAX_SAVED_NETWORKS - 1; j++) {
        char srcS[16], srcP[16], dstS[16], dstP[16];
        snprintf(srcS, sizeof(srcS), "wifi_ssid_%d", j + 1);
        snprintf(srcP, sizeof(srcP), "wifi_pass_%d", j + 1);
        snprintf(dstS, sizeof(dstS), "wifi_ssid_%d", j);
        snprintf(dstP, sizeof(dstP), "wifi_pass_%d", j);
        wifiPrefs.putString(dstS, wifiPrefs.getString(srcS, ""));
        wifiPrefs.putString(dstP, wifiPrefs.getString(srcP, ""));
      }
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

  WiFi.mode(WIFI_STA);

  // DO NOT call WiFi.setSleep(false) here. On the X4 (ESP32-C3, WiFi+BLE
  // sharing one radio) that call aborted the firmware outright:
  //   E wifi:Error! Should enable WiFi modem sleep when both WiFi and
  //     Bluetooth are enabled!!!!!!
  //   abort() was called at PC ... on core 0
  // The ESP32-S3 has the same single-radio WiFi/BLE coexistence
  // constraint, and this port's own BLE keyboard (freeink-sdk's
  // BleKeyboardHost) can very plausibly be connected at the same time
  // someone opens this screen -- so the same rule applies here until
  // proven otherwise on real hardware. Modem sleep stays on; the cost is
  // added latency (the station only wakes on the AP's beacon interval),
  // not a crash.
  WiFi.disconnect(true);

  // scanNetworks() reports failure immediately rather than through
  // scanComplete(), and the two failures look identical on screen
  // otherwise. Worth distinguishing: bringing up the WiFi stack is a real
  // heap allocation, and this firmware also runs NimBLE for the keyboard.
  const int16_t started = WiFi.scanNetworks(true);
  Serial.printf("[wifi] scan start=%d heap=%u largest=%u\n", (int)started,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  if (started == WIFI_SCAN_FAILED) {
    syncState = SyncState::NETWORK_LIST;
    snprintf(statusText, sizeof(statusText), "Radio busy (heap %uK)",
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024));
  }

  screenDirty = true;
}

static void processScanResults() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;  // Still scanning

  if (n <= 0) {
    networkCount = 0;
    syncState = SyncState::NETWORK_LIST;
    snprintf(statusText, sizeof(statusText), "%s",
             n == 0 ? "No networks found" : "Scan failed");
    WiFi.scanDelete();
    screenDirty = true;
    return;
  }

  // Deduplicate by SSID, keeping strongest signal.
  networkCount = 0;
  for (int i = 0; i < n && networkCount < MAX_NETWORKS; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;  // Skip hidden networks

    bool duplicate = false;
    for (int j = 0; j < networkCount; j++) {
      if (strcmp(networks[j].ssid, ssid.c_str()) == 0) {
        duplicate = true;
        if (WiFi.RSSI(i) > networks[j].rssi) networks[j].rssi = WiFi.RSSI(i);
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
  loadSavedCredentials();

  // Sort: saved networks first, then by signal strength.
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
  Serial.printf("[wifi] found %d network(s)\n", networkCount);

  // Auto-connect to the strongest available saved network. If that one's
  // password has since changed, pollConnection()'s timeout routes to
  // FORGET_PROMPT, and answering "forget" re-scans -- which lands here
  // again and tries whichever saved network is now strongest, so a
  // changed password on one saved network doesn't block the others.
  if (networkCount > 0 && networks[0].saved) {
    char savedPass[MAX_PASSWORD_LEN + 1];
    if (getSavedPassword(networks[0].ssid, savedPass, sizeof(savedPass))) {
      usedSavedPassword = true;
      autoConnectAttempted = true;
      beginConnect(networks[0].ssid, savedPass);
      Serial.printf("[wifi] auto-connecting to strongest saved network: %s\n", networks[0].ssid);
    }
  }
}

// =========================================================================
// Connection
// =========================================================================

// Confirmed on hardware, and matches a documented ESP32-S3 errata: the STA
// connect handshake fails outright -- "wifi:Coexist: Wi-Fi connect fail,
// apply reconnect coex policy" -- whenever a BLE connection is live at the
// same time, active traffic or not (a merely-idle bonded link was enough
// to reproduce it here). NimBLE.end() fully tears the BLE stack down for
// the duration of the connect attempt; poll()'s own auto-reconnect brings
// the keyboard back once WiFi.begin() has actually connected or given up
// (see pollConnection()'s two exit paths below). The on-screen keyboard
// stays available throughout regardless (forced visible for the whole
// WiFi session -- see main.cpp's SYNC tap handler), so this brief gap
// never leaves the wizard with no way to type.
// BLE now only starts on the status bar's BLE button (see main.cpp's
// forceBlePairingNow()), not automatically at boot -- so a WiFi attempt made
// before anyone has tapped it must not itself bring the stack up for the
// first time. This remembers whether BLE was actually running at the moment
// it got suspended, and only that state gets restored.
static bool g_bleWasRunningBeforeSuspend = false;

static void suspendBleForWifiConnect() {
  g_bleWasRunningBeforeSuspend = BleHid.isRunning();
  if (g_bleWasRunningBeforeSuspend) BleHid.end();
}

static void resumeBleAfterWifiConnect() {
  if (g_bleWasRunningBeforeSuspend && !BleHid.isRunning()) {
    BleHid.begin("MicroBASIC-PaperS3");
  }
}

static void beginConnect(const char* ssid, const char* pass) {
  strncpy(connectingSSID, ssid, 32);
  connectingSSID[32] = '\0';
  syncState = SyncState::CONNECTING;
  snprintf(statusText, sizeof(statusText), "Connecting to %s...", ssid);
  connectStartMs = millis();

  suspendBleForWifiConnect();
  WiFi.disconnect(true);
  delay(50);
  WiFi.begin(ssid, pass);
  screenDirty = true;
  Serial.printf("[wifi] connecting to %s\n", ssid);
}

static void enterSyncingState() {
  resetTransferTracking();
  startHttpServer();
  // Shown as a ready-to-type browser URL, not a bare IP: mDNS names don't
  // resolve on every network/OS, and the numeric IP is the one address
  // that always works regardless.
  snprintf(statusText, sizeof(statusText), "http://%s/", WiFi.localIP().toString().c_str());
  syncState = SyncState::SYNCING;
  lastHttpActivityMs = millis();
  screenDirty = true;
  Serial.printf("[wifi] serving at %s\n", statusText);
}

static void enterDoneState() {
  stopHttpServer();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  syncState = SyncState::DONE;
  doneStartMs = millis();

  if (filesSent == 0 && filesReceived == 0) {
    strcpy(statusText, "No changes");
  } else {
    snprintf(statusText, sizeof(statusText), "Sent: %d  Received: %d", filesSent, filesReceived);
  }
  screenDirty = true;
  Serial.printf("[wifi] done -- %s\n", statusText);
}

// The two states that put a *question* on screen. Both are entered by
// something finishing rather than by the user pressing anything, so
// whatever's already queued (the Enter that submitted the password, or a
// repeat of it) belongs to a different screen and is discarded on entry.
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
    resumeBleAfterWifiConnect();
    if (!usedSavedPassword) {
      snprintf(statusText, sizeof(statusText), "%s", WiFi.localIP().toString().c_str());
      openPrompt(SyncState::SAVE_PROMPT);
    } else {
      enterSyncingState();
    }
    return;
  }

  if (millis() - connectStartMs > 25000) {
    WiFi.disconnect(true);
    resumeBleAfterWifiConnect();
    strcpy(statusText, "Connection failed");

    if (usedSavedPassword) {
      openPrompt(SyncState::FORGET_PROMPT);
    } else {
      syncState = SyncState::CONNECT_FAILED;
      screenDirty = true;
    }
    Serial.println("[wifi] connection timed out");
  }
}

// =========================================================================
// HTTP Server
// =========================================================================

static constexpr char PROGRAMS_DIR[] = "/MicroBASIC/programs";  // matches tb_runtime.cpp's TB_DIR

// Which upload is in flight. Set at UPLOAD_FILE_START, read by the later
// WRITE/END callbacks (which have no access to the request's query string).
static bool uploading = false;

static void handleFileList() {
  lastHttpActivityMs = millis();
  if (!pcConnected) {
    pcConnected = true;
    screenDirty = true;
  }

  auto dir = SdMan.open(PROGRAMS_DIR);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    // An absent directory is normal (nothing saved yet), not an error.
    server->send(200, "application/json", "[]");
    return;
  }

  String json = "[";
  bool first = true;
  char name[256];

  dir.rewindDirectory();
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.' || file.isDirectory()) { file.close(); continue; }

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

// Serves /programs/<filename>. Returns false if the URI isn't shaped like
// that (falls through to a plain 404 in handleNotFound()).
static bool handleFileDownload() {
  lastHttpActivityMs = millis();

  String uri = server->uri();
  static constexpr char kPrefix[] = "/programs/";
  if (!uri.startsWith(kPrefix) || uri.length() <= strlen(kPrefix)) return false;

  String filename = uri.substring(strlen(kPrefix));
  if (filename.indexOf("..") >= 0 || filename.indexOf('/') >= 0) {
    server->send(400, "text/plain", "Invalid name");
    return true;
  }

  char path[320];
  snprintf(path, sizeof(path), "%s/%s", PROGRAMS_DIR, filename.c_str());

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

  filesSent++;
  screenDirty = true;
  Serial.printf("[wifi] sent file: %s\n", path);
  return true;
}

static void handleFilesPage() {
  lastHttpActivityMs = millis();

  // Two things this deliberately does not do. It does not call send() with
  // the page as a String: that copies the whole page into one contiguous
  // allocation on a device that's also running NimBLE. And it does not hand
  // the whole thing to one write() either -- WiFiClient::write() gives up
  // after a fixed number of retries, and a body that runs out of retries
  // isn't an error anyone sees, it's a page that renders with a script tag
  // that never arrived. Sent a TCP segment at a time instead, so each chunk
  // gets its own retry budget.
  const size_t len = strlen(FILES_PAGE_HTML);
  server->setContentLength(len);
  server->send(200, "text/html", "");

  constexpr size_t CHUNK = 1440;  // one segment at the default MSS
  for (size_t off = 0; off < len; off += CHUNK) {
    const size_t n = (len - off < CHUNK) ? (len - off) : CHUNK;
    server->sendContent_P(FILES_PAGE_HTML + off, n);
  }
}

// The file manager lives at "/" -- redirect anyone with "/files" bookmarked
// or typed from muscle memory (this project's own port-staging carried the
// same redirect for the same reason).
static void handleFilesPageRedirect() {
  lastHttpActivityMs = millis();
  server->sendHeader("Location", "/");
  server->send(302, "text/plain", "");
}

// Called repeatedly with chunks of the multipart body as they arrive.
static void handleUploadData() {
  HTTPUpload& up = server->upload();
  lastHttpActivityMs = millis();

  if (up.status == UPLOAD_FILE_START) {
    if (!pcConnected) { pcConnected = true; screenDirty = true; }
    uploading = true;

    // Filename only (no path), clamped length. No forced extension --
    // programs are stored under exactly the name given, matching SAVE
    // (see screen_editor.h / tb_runtime.cpp's tbPath()).
    String filename = up.filename;
    int slash = filename.lastIndexOf('/');
    if (slash >= 0) filename = filename.substring(slash + 1);
    if (filename.length() == 0) filename = "upload";
    if ((int)filename.length() > MAX_FILENAME_LEN - 1) filename = filename.substring(0, MAX_FILENAME_LEN - 1);

    if (!SdMan.exists("/MicroBASIC")) SdMan.mkdir("/MicroBASIC");
    if (!SdMan.exists(PROGRAMS_DIR)) SdMan.mkdir(PROGRAMS_DIR);

    snprintf(upload_.path, sizeof(upload_.path), "%s/%s", PROGRAMS_DIR, filename.c_str());
    upload_.file = SdMan.open(upload_.path, O_WRONLY | O_CREAT | O_TRUNC);
    upload_.bytesWritten = 0;
    upload_.tooLarge = false;
    upload_.ok = (bool)upload_.file;
    Serial.printf("[wifi] upload start: %s\n", upload_.path);

  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!upload_.ok) return;
    if (upload_.bytesWritten + up.currentSize > PROGRAM_UPLOAD_MAX_SIZE - 1) {
      upload_.ok = false;
      upload_.tooLarge = true;
      upload_.file.close();
      SdMan.remove(upload_.path);
      return;
    }
    size_t written = upload_.file.write(up.buf, up.currentSize);
    upload_.bytesWritten += written;
    if (written != up.currentSize) upload_.ok = false;

  } else if (up.status == UPLOAD_FILE_END) {
    if (upload_.file) upload_.file.close();
    if (upload_.ok) {
      filesReceived++;
      screenDirty = true;
      Serial.printf("[wifi] upload complete: %s (%u bytes)\n", upload_.path,
                     (unsigned)upload_.bytesWritten);
    }

  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (upload_.file) upload_.file.close();
    upload_.ok = false;
  }
}

// Called once after the whole request body has been consumed -- sends the response.
static void handleUploadDone() {
  lastHttpActivityMs = millis();
  if (upload_.ok) {
    server->send(200, "text/plain", "OK");
  } else if (upload_.tooLarge) {
    char msg[64];
    snprintf(msg, sizeof(msg), "File too large (%uKB max)", (unsigned)((PROGRAM_UPLOAD_MAX_SIZE) / 1024));
    server->send(400, "text/plain", msg);
  } else {
    server->send(400, "text/plain", "Upload failed");
  }
  uploading = false;
}

static void handleDeleteFile() {
  lastHttpActivityMs = millis();
  if (!server->hasArg("name")) {
    server->send(400, "text/plain", "Missing name");
    return;
  }
  String name = server->arg("name");
  if (name.length() == 0 || name.indexOf('/') >= 0 || name.indexOf("..") >= 0) {
    server->send(400, "text/plain", "Invalid name");
    return;
  }
  char path[320];
  snprintf(path, sizeof(path), "%s/%s", PROGRAMS_DIR, name.c_str());
  if (!SdMan.exists(path)) {
    server->send(404, "text/plain", "Not found");
    return;
  }
  SdMan.remove(path);
  server->send(200, "text/plain", "OK");
  screenDirty = true;
  Serial.printf("[wifi] deleted via browser: %s\n", path);
}

static void handleNotFound() {
  if (server->method() == HTTP_GET && handleFileDownload()) return;
  server->send(404, "text/plain", "Not found");
}

static void startHttpServer() {
  if (server) return;
  server = new WebServer(80);
  server->on("/api/files", HTTP_GET, handleFileList);
  server->on("/", HTTP_GET, handleFilesPage);
  server->on("/files", HTTP_GET, handleFilesPageRedirect);
  server->on("/upload", HTTP_POST, handleUploadDone, handleUploadData);
  server->on("/delete", HTTP_POST, handleDeleteFile);
  server->onNotFound(handleNotFound);
  server->begin();
  MDNS.begin("microbasic-papers3");
  Serial.printf("[wifi] HTTP server at %s heap=%u largest=%u\n", WiFi.localIP().toString().c_str(),
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
// Input handling -- routed here by main.cpp whenever isWifiSyncActive()
// =========================================================================

void syncHandleKey(uint8_t keyCode, uint8_t modifiers) {
  switch (syncState) {
    case SyncState::SCANNING:
      if (keyCode == HID_KEY_ESCAPE) wifiSyncStop();
      break;

    case SyncState::NETWORK_LIST:
      if (keyCode == HID_KEY_DOWN && networkCount > 0) {
        selectedNet = (selectedNet + 1) % networkCount;
        screenDirty = true;
      } else if (keyCode == HID_KEY_UP && networkCount > 0) {
        selectedNet = (selectedNet - 1 + networkCount) % networkCount;
        screenDirty = true;
      } else if (keyCode == HID_KEY_ENTER && networkCount > 0) {
        char savedPass[MAX_PASSWORD_LEN + 1];
        if (getSavedPassword(networks[selectedNet].ssid, savedPass, sizeof(savedPass))) {
          usedSavedPassword = true;
          autoConnectAttempted = false;
          beginConnect(networks[selectedNet].ssid, savedPass);
        } else if (!networks[selectedNet].encrypted) {
          usedSavedPassword = false;
          autoConnectAttempted = false;
          beginConnect(networks[selectedNet].ssid, "");
        } else {
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
        if (passwordLen > 0) beginConnect(networks[selectedNet].ssid, passwordBuf);
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
        const char c = hidToAscii(keyCode, modifiers);
        if (c != 0 && c >= ' ' && c != '\n' && c != '\t' && passwordLen < MAX_PASSWORD_LEN) {
          passwordBuf[passwordLen++] = c;
          passwordBuf[passwordLen] = '\0';
          screenDirty = true;
        }
      }
      break;

    case SyncState::CONNECTING:
      if (keyCode == HID_KEY_ESCAPE) {
        WiFi.disconnect(true);
        resumeBleAfterWifiConnect();
        if (autoConnectAttempted) {
          beginScan();
        } else {
          syncState = SyncState::NETWORK_LIST;
          screenDirty = true;
        }
      }
      break;

    case SyncState::SYNCING:
      if (keyCode == HID_KEY_ESCAPE) wifiSyncStop();
      break;

    case SyncState::DONE:
      wifiSyncStop();  // any key returns to the editor immediately
      break;

    case SyncState::CONNECT_FAILED:
      if (keyCode == HID_KEY_ENTER) {
        beginScan();
      } else if (keyCode == HID_KEY_ESCAPE) {
        wifiSyncStop();
      }
      break;

    case SyncState::SAVE_PROMPT:
      if (promptStillSettling()) break;
      // Up = Yes (save), Down = No (skip)
      if (keyCode == HID_KEY_UP || keyCode == HID_KEY_ENTER) {
        saveCredential(connectingSSID, passwordBuf);
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
        beginScan();
      } else if (keyCode == HID_KEY_DOWN || keyCode == HID_KEY_ESCAPE) {
        // Keep credentials, but land on the list rather than re-triggering
        // the same auto-connect immediately.
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
  // Statics that outlive a session, and a session can end in any state --
  // including one that set them. A stale usedSavedPassword suppresses the
  // "save password?" prompt on the next manual connect.
  usedSavedPassword = false;
  autoConnectAttempted = false;
  wifiPrefs.begin("wifi_creds", false);
  if (!wifiPrefs.isKey("wifi_count")) restoreWifiBackup();
  resetTransferTracking();

  beginScan();
  Serial.println("[wifi] setup started");
}

void wifiSyncStop() {
  if (!syncActive) return;

  stopHttpServer();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  // Safety net: every path that suspends BLE for a connect attempt also
  // resumes it on its own way out, but wifiSyncStop() can be reached from
  // states that never suspended it at all (e.g. Escape from NETWORK_LIST)
  // as well as ones that did -- isRunning() makes this a no-op in the
  // former case rather than needing every caller to track which is which.
  resumeBleAfterWifiConnect();

  wifiPrefs.end();
  syncActive = false;
  networkCount = 0;
  passwordBuf[0] = '\0';
  passwordLen = 0;
  statusText[0] = '\0';
  screenDirty = true;

  Serial.println("[wifi] setup stopped");
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
      if (millis() - lastHttpActivityMs > SYNC_TIMEOUT_MS) {
        Serial.println("[wifi] idle timeout -- no HTTP activity for 5 min");
        enterDoneState();
      }
      break;

    case SyncState::DONE:
      if (millis() - doneStartMs > DONE_DISPLAY_MS) wifiSyncStop();
      break;

    default:
      break;
  }
}

bool isWifiSyncActive() { return syncActive; }
SyncState getSyncState() { return syncState; }
int getNetworkCount() { return networkCount; }

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
int getSelectedNetwork() { return selectedNet; }
const char* getPasswordBuffer() { return passwordBuf; }
int getPasswordLen() { return passwordLen; }
const char* getSyncStatusText() { return statusText; }
int getSyncFilesSent() { return filesSent; }
int getSyncFilesReceived() { return filesReceived; }
bool isPcConnected() { return pcConnected; }
