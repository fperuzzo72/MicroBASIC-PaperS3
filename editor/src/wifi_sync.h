#pragma once

#include <cstdint>

// Ported from MicroBASIC's own wifi_sync.h (editor/port-staging/). Trimmed:
// no DONE-state MicroSlate handshake (that project's own PC-side sync
// script isn't part of this port -- see wifi_sync.cpp's own header comment
// for what that means concretely), and this device serves one file
// collection (BASIC programs), not two, so there's no per-tab plumbing to
// expose here.

// State machine states.
enum class SyncState : uint8_t {
  SCANNING,
  NETWORK_LIST,
  PASSWORD_ENTRY,
  CONNECTING,
  SYNCING,         // Server running, tracking transfers
  DONE,            // Summary shown, WiFi off, auto-return to the editor
  CONNECT_FAILED,
  SAVE_PROMPT,
  FORGET_PROMPT
};

// Lifecycle
void wifiSyncStart();       // Begin scanning (or auto-connect if saved creds)
void wifiSyncStop();        // Stop everything, WiFi off, back to the editor
void wifiSyncLoop();        // Poll scan/connection/HTTP -- call every loop() iteration
bool isWifiSyncActive();

// For the WiFi UI screen (main.cpp)
SyncState getSyncState();
int  getNetworkCount();
const char* getNetworkSSID(int i);
int  getNetworkRSSI(int i);
bool isNetworkEncrypted(int i);
bool isNetworkSaved(int i);
int  getSelectedNetwork();
const char* getPasswordBuffer();
int  getPasswordLen();
const char* getSyncStatusText();

// Transfer activity, for the SYNCING-state screen.
int  getSyncFilesSent();
int  getSyncFilesReceived();
bool isPcConnected();

// For input handling -- routed here instead of the screen editor whenever
// isWifiSyncActive() is true (see main.cpp's key dispatch).
void syncHandleKey(uint8_t keyCode, uint8_t modifiers);
