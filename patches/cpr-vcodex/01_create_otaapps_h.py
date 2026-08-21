"""Cria cpr-vcodex/src/util/OtaApps.h — arquivo novo, sem conflito com o CPR-vCodex.

Reaproveita o src/network/OtaBootSwitch.h do próprio CPR-vCodex (usado
pelo autoupdate SD/OTA nativo dele), que já contorna o bug de
esp_ota_set_boot_partition (erro de verificação de efuse-blk-rev) nesse
hardware — não precisamos reescrever essa parte. Igual à versão para
CrossInk/CrossPoint, com uma função a mais (switchToFirstOtaApp) usada
pelo atalho "MicroSlate" — ver 02_patch_shortcut_registry.py.
"""

OTA_APPS_H = r'''#pragma once

// Dual-boot sibling-app detection and switching. The register/detect/switch
// scheme is ported from MicroSlate (github.com/Josh-writes/microslate-
// firmware, src/main.cpp), itself adapted from CrossInk's build-time
// dual-boot patch (uxjulia/CrossInk). The low-level otadata-write primitive
// (ota_boot::switchTo) is NOT ported: CPR-vCodex already ships the same fix
// natively as src/network/OtaBootSwitch.h/.cpp (used by its own SD/OTA
// self-update), so this reuses that instead of duplicating it.

#include <Preferences.h>
#include <esp_ota_ops.h>

#include <cstdio>
#include <cstring>

#include "network/OtaBootSwitch.h"

static constexpr int MAX_OTA_APPS = 4;

struct OtaAppEntry {
  char name[32];
  int partitionSubtype;
};

// Records this app's display name in shared NVS, keyed by which OTA slot
// it's currently running from, so sibling apps can show it by name.
inline void registerOtaAppName(const char* name) {
  const esp_partition_t* self = esp_ota_get_running_partition();
  if (!self) return;
  const int slot = self->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0;
  char key[8];
  snprintf(key, sizeof(key), "ota_%d", slot);
  Preferences prefs;
  prefs.begin("ota_names", false);
  prefs.putString(key, name);
  prefs.end();
}

// Scans every OTA app partition except the one currently running and fills
// `apps[]` with their registered names (falling back to "OTA Slot N" for an
// unregistered/never-booted sibling). Returns the number found.
inline int detectOtaApps(OtaAppEntry* apps, int maxApps) {
  int count = 0;
  const esp_partition_t* running = esp_ota_get_running_partition();
  Preferences prefs;
  prefs.begin("ota_names", true);

  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (it && count < maxApps) {
    const esp_partition_t* p = esp_partition_get(it);
    if (p && p != running && p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
        p->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_15) {
      esp_app_desc_t desc;
      if (esp_ota_get_partition_description(p, &desc) == ESP_OK) {
        const int slot = p->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0;
        char key[8];
        snprintf(key, sizeof(key), "ota_%d", slot);
        String nvsName = prefs.getString(key, "");

        OtaAppEntry& entry = apps[count];
        if (nvsName.length() > 0) {
          strncpy(entry.name, nvsName.c_str(), sizeof(entry.name) - 1);
        } else {
          snprintf(entry.name, sizeof(entry.name), "OTA Slot %d", slot);
        }
        entry.name[sizeof(entry.name) - 1] = '\0';
        entry.partitionSubtype = p->subtype;
        count++;
      }
    }
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);
  prefs.end();
  return count;
}

// ota_boot::switchTo() always marks the newly-selected slot's otadata state
// as "new" (pending verify) — correct for this reader's own genuine
// firmware self-update (FirmwareFlasher.cpp reuses this same switchTo(),
// where the bootloader's rollback safety net should apply to fresh,
// untested code), but wrong for a plain dual-boot switch: we only ever
// point at an *already-flashed, previously-working* sibling slot, never at
// new code. Left as "new", the very next reset before the app confirms
// itself (neither app calls esp_ota_mark_app_valid_cancel_rollback())
// gets silently rolled back by the bootloader to the other slot — observed
// as waking from sleep back into the previous app even though the sibling
// was active when it went to sleep. This finds the entry switchTo() just
// wrote (highest seq) and flips just its state to valid, leaving
// switchTo() itself — and so the reader's own self-update path — with full
// rollback protection intact.
inline void confirmLastOtaSwitch() {
  const esp_partition_t* otadata =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (!otadata) return;
  constexpr size_t kSectorSize = 0x1000;
  if (otadata->size < 2 * kSectorSize) return;

  ota_boot::SelectEntry slots[2] = {};
  if (esp_partition_read(otadata, 0, &slots[0], sizeof(ota_boot::SelectEntry)) != ESP_OK ||
      esp_partition_read(otadata, kSectorSize, &slots[1], sizeof(ota_boot::SelectEntry)) != ESP_OK) {
    return;
  }

  int newestIdx = -1;
  uint32_t newestSeq = 0;
  for (int i = 0; i < 2; ++i) {
    if (slots[i].ota_seq == 0xFFFFFFFFu) continue;
    if (slots[i].crc != ota_boot::computeSeqCrc(slots[i].ota_seq)) continue;
    if (newestIdx < 0 || slots[i].ota_seq > newestSeq) {
      newestIdx = i;
      newestSeq = slots[i].ota_seq;
    }
  }
  if (newestIdx < 0) return;

  constexpr uint32_t kOtaImgValid = 2;  // ESP_OTA_IMG_VALID
  slots[newestIdx].ota_state = kOtaImgValid;

  const size_t off = static_cast<size_t>(newestIdx) * kSectorSize;
  if (esp_partition_erase_range(otadata, off, kSectorSize) != ESP_OK) return;
  esp_partition_write(otadata, off, &slots[newestIdx], sizeof(slots[newestIdx]));
}

// Switches the boot partition to `subtype` and restarts. Uses
// ota_boot::switchTo (writes otadata directly) instead of
// esp_ota_set_boot_partition, which fails on this hardware with a bogus
// efuse-blk-rev verification error.
inline void switchToOtaApp(int subtype) {
  const esp_partition_t* target =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, static_cast<esp_partition_subtype_t>(subtype), nullptr);
  if (!target) return;
  if (ota_boot::switchTo(target)) {
    confirmLastOtaSwitch();
    esp_restart();
  }
}

// Convenience for the "MicroSlate" shortcut: this firmware only ever has one
// dual-boot sibling, so just switch to whichever OTA app is detected first
// (a no-op if the other slot has never been flashed).
inline void switchToFirstOtaApp() {
  OtaAppEntry apps[MAX_OTA_APPS];
  const int count = detectOtaApps(apps, MAX_OTA_APPS);
  if (count > 0) {
    switchToOtaApp(apps[0].partitionSubtype);
  }
}
'''

with open("cpr-vcodex/src/util/OtaApps.h", "w") as f:
    f.write(OTA_APPS_H)

print("OtaApps.h: OK")
