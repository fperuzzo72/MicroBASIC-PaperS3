#include <Arduino.h>
#include <cstring>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <GfxRenderer.h>
#include <esp_pm.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include "OtaBootSwitch.h"
#include <Preferences.h>
#include "sd_backup.h"
#include "Utf8.h"

#include "config.h"
#include "ble_keyboard.h"
#include "input_handler.h"
#include "text_editor.h"
#include "file_manager.h"
#include "ui_renderer.h"
#include "wifi_sync.h"
#include "sd_datetime.h"

#include <sys/time.h>  // settimeofday(), na entrega para outra particao
#include "screen_editor.h"
#include "tb_bridge.h"

// Enum for sleep reasons
enum class SleepReason {
  POWER_LONGPRESS,
  IDLE_TIMEOUT,
  MENU_ACTION
};

// Forward declarations
void renderSleepScreen();
void enterDeepSleep(SleepReason reason);

// External variables
extern bool autoReconnectEnabled;

// --- Hardware objects ---
HalDisplay display;
GfxRenderer renderer(display);
HalGPIO gpio;

// Applies a config::Orientation to the renderer's GfxRenderer::Orientation.
// Used both for the persisted app-wide setting (currentOrientation, see
// below) and for SCREEN_EDITOR's own orientation override, which must NOT
// touch currentOrientation — it's a temporary force while that mode is
// active, restored on exit, and must never get persisted to NVS/SD as if
// the user had changed their real setting.
void applyOrientationToRenderer(Orientation o) {
  GfxRenderer::Orientation gfxOrient = GfxRenderer::Portrait;
  switch (o) {
    case Orientation::PORTRAIT:      gfxOrient = GfxRenderer::Portrait; break;
    case Orientation::LANDSCAPE_CW:  gfxOrient = GfxRenderer::LandscapeClockwise; break;
    case Orientation::PORTRAIT_INV:  gfxOrient = GfxRenderer::PortraitInverted; break;
    case Orientation::LANDSCAPE_CCW: gfxOrient = GfxRenderer::LandscapeCounterClockwise; break;
  }
  renderer.setOrientation(gfxOrient);
}


// --- Persistent settings (NVS) ---
static Preferences uiPrefs;

// --- Shared UI state ---
// Boots straight into MicroBASIC (SCREEN 1) -- the physical Back button
// (and the typed MENU command) is the deliberate, always-available way
// back to MAIN_MENU. See docs/DEVELOPMENT_LOG.md.
UIState currentState = UIState::SCREEN_EDITOR;
int mainMenuSelection = 0;
int selectedFileIndex = 0;
int settingsSelection = 0;
int bluetoothDeviceSelection = 0;
int pairedKeyboardSelection = 0;
Orientation currentOrientation = Orientation::PORTRAIT;
bool screenDirty = true;

// Rename buffer
char renameBuffer[MAX_FILENAME_LEN] = "";
int renameBufferLen = 0;

// UI mode flags
bool darkMode = false;
bool cleanMode = false;
bool deleteConfirmPending = false;
WritingMode writingMode = WritingMode::NORMAL;
FontSize fontSize = FontSize::LARGE;
bool showWordCount = true;

// Gira os botoes fisicos 90 graus para os programas BASIC. O editor de tela
// roda em paisagem enquanto o d-pad esta fisicamente na lateral, entao num
// jogo os botoes apontam para as direcoes erradas do campo. Vale SO enquanto
// um programa executa: nos menus e nos editores a consistencia da interface
// importa mais do que a geografia dos botoes.
bool remapButtonsInPrograms = false;

// --- OTA App Detection ---
OtaAppEntry otaApps[MAX_OTA_APPS];
int otaAppCount = 0;

// Register this app's display name in shared NVS, keyed by OTA slot number.
static void registerOtaAppName(const char* name) {
  const esp_partition_t* self = esp_ota_get_running_partition();
  if (!self) return;
  int slot = self->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0;
  char key[8];
  snprintf(key, sizeof(key), "ota_%d", slot);
  Preferences prefs;
  prefs.begin("ota_names", false);
  prefs.putString(key, name);
  prefs.end();
  DBG_PRINTF("[OTA] Registered as \"%s\" in slot %d\n", name, slot);
}

// Scan all OTA partitions (except self), check for valid firmware, populate otaApps[].
static void detectOtaApps() {
  otaAppCount = 0;
  const esp_partition_t* running = esp_ota_get_running_partition();
  Preferences otaPrefs;
  otaPrefs.begin("ota_names", true);  // read-only

  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP,
                                                    ESP_PARTITION_SUBTYPE_ANY, NULL);
  while (it != NULL && otaAppCount < MAX_OTA_APPS) {
    const esp_partition_t* part = esp_partition_get(it);
    if (part && part != running
        && part->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0
        && part->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_15) {

      esp_app_desc_t desc;
      if (esp_ota_get_partition_description(part, &desc) == ESP_OK) {
        int slot = part->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0;
        char key[8];
        snprintf(key, sizeof(key), "ota_%d", slot);
        String nvsName = otaPrefs.getString(key, "");

        OtaAppEntry& entry = otaApps[otaAppCount];
        if (nvsName.length() > 0) {
          strncpy(entry.name, nvsName.c_str(), sizeof(entry.name) - 1);
        } else {
          snprintf(entry.name, sizeof(entry.name), "OTA Slot %d", slot);
        }
        entry.name[sizeof(entry.name) - 1] = '\0';
        entry.partitionSubtype = part->subtype;
        otaAppCount++;
      }
    }
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);
  otaPrefs.end();
  DBG_PRINTF("[OTA] Detected %d additional app(s)\n", otaAppCount);
}

// ota_boot::switchTo() always marks the newly-selected slot's otadata state
// as "new" (pending verify) — correct for a genuine firmware self-update
// (fresh, untested code, where the bootloader's rollback safety net should
// apply), but wrong for a plain dual-boot switch: we only ever point at an
// *already-flashed, previously-working* sibling slot, never at new code.
// Left as "new", the very next reset before the app confirms itself
// (neither this editor nor any reader calls
// esp_ota_mark_app_valid_cancel_rollback()) gets silently rolled back by
// the bootloader to the other slot — observed as waking from sleep back
// into the reader even though the editor was active when it went to sleep.
// This finds the entry switchTo() just wrote (highest seq) and flips just
// its state to valid, leaving switchTo() itself untouched — so a reader's
// own genuine self-update (which reuses the exact same function) still
// gets full rollback protection. Ported from MicroWriter's own fix
// (commit 27b2f65) -- see docs/DEVELOPMENT_LOG.md.
static void confirmLastOtaSwitch() {
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

// Switch to another OTA app by index into otaApps[]. Non-static so input_handler can call it.
void switchToOtaApp(int index) {
  if (index < 0 || index >= otaAppCount) return;
  int subtype = otaApps[index].partitionSubtype;
  const esp_partition_t* target = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP,
      static_cast<esp_partition_subtype_t>(subtype), NULL);
  if (!target) {
    DBG_PRINTF("[OTA] Partition subtype %d not found!\n", subtype);
    return;
  }
  DBG_PRINTF("[OTA] Switching to \"%s\" (subtype %d)...\n", otaApps[index].name, subtype);
  // esp_ota_set_boot_partition() fails on the X4/X3 with a bogus efuse-blk-rev
  // verify error (same issue documented in CrossInk's OtaBootSwitch.h). We
  // write the otadata selection directly instead, matching what the web
  // flasher and CrossInk itself do.
  if (!ota_boot::switchTo(target)) {
    DBG_PRINTF("[OTA] switchTo failed, staying on current app\n");
    return;
  }
  confirmLastOtaSwitch();

  // Zera o relogio de sistema antes de entregar o controle.
  //
  // Sem DS3231 (o X4 nao tem: HalClock::begin() desiste se nao for um X3), o
  // CPR-vCodex guarda o dia em /.crosspoint/state.json no cartao, no campo
  // lastKnownValidTimestamp, e o atualiza assim:
  //
  //     lastKnownValidTimestamp =
  //         std::max(lastKnownValidTimestamp, getCurrentValidTimestamp());
  //
  // e considera valido qualquer relogio ACIMA de 2024-01-01 -- so rejeita
  // valores pequenos demais, nunca grandes demais.
  //
  // Numa troca de particao, o relogio de sistema volta com lixo: a referencia
  // do ESP-IDF (s_boot_time) fica na memoria RTC, cujo layout o linker define
  // por binario, e nos declaramos zero variaveis ali enquanto o leitor declara
  // onze. O lixo observado foi 4154457600 -- 26/08/2101. Sendo maior que 2024,
  // passa na validacao, o max o adota, e ele fica **gravado no cartao**. Como
  // e max, nunca mais desce sozinho: so o "Set date" manual conserta, porque
  // aquele caminho atribui em vez de comparar.
  //
  // Zerando aqui, getCurrentValidTimestamp() devolve 0, o max fica com o valor
  // que ja estava no arquivo, e a data guardada e **preservada**. Nao se perde
  // sincronia -- apenas este boot nao conta como sincronizado, que ja e o
  // normal num X4 depois de qualquer evento de energia.
  //
  // Isto e mitigacao, nao correcao: a causa esta no layout da memoria RTC
  // diferir entre dois binarios, e disso nao temos como escapar. O que da para
  // fazer e nao entregar um numero que a heuristica do outro lado aceite.
  struct timeval tv = {};
  settimeofday(&tv, nullptr);

  esp_restart();
}

// --- Screen update ---
static void updateScreen() {
  if (!screenDirty) return;
  screenDirty = false;

  // Apply orientation. SCREEN_EDITOR forces/restores its own orientation
  // override directly (entry point in input_handler.cpp, boot path in
  // setup()) without touching currentOrientation -- this has to actively
  // skip re-applying currentOrientation while that mode is active, not
  // just rely on currentOrientation staying unchanged: since boot now
  // starts in SCREEN_EDITOR (see main.cpp's currentState initializer),
  // lastOrientation's PORTRAIT default can differ from a real saved
  // non-Portrait currentOrientation on the very first call here, which
  // would otherwise stomp the boot-time LANDSCAPE_CCW override immediately.
  static Orientation lastOrientation = Orientation::PORTRAIT;
  if (currentOrientation != lastOrientation && currentState != UIState::SCREEN_EDITOR) {
    applyOrientationToRenderer(currentOrientation);
    lastOrientation = currentOrientation;
  }

  // Word-wrap budget in pixels, matching drawEditorLine's own maxW (sw - 20,
  // see ui_renderer.cpp) exactly. text_editor.cpp wraps by summing each
  // line's real glyph widths (via the editorGlyphWidthPx callback registered
  // in rendererSetup) against this budget, not an estimated average — so a
  // line can never render wider than what was actually budgeted for it,
  // whatever mix of narrow/wide or accented glyphs it contains.
  editorSetMaxLineWidthPx(renderer.getScreenWidth() - 20);

  switch (currentState) {
    case UIState::MAIN_MENU:         drawMainMenu(renderer, gpio); break;
    case UIState::FILE_BROWSER:      drawFileBrowser(renderer, gpio); break;
    case UIState::TEXT_EDITOR:       drawTextEditor(renderer, gpio); break;
    case UIState::RENAME_FILE:       drawRenameScreen(renderer, gpio); break;
    case UIState::SETTINGS:          drawSettingsMenu(renderer, gpio); break;
    case UIState::BLUETOOTH_SETTINGS: drawBluetoothSettings(renderer, gpio); break;
    case UIState::PAIRED_KEYBOARDS:   drawPairedKeyboardsMenu(renderer, gpio); break;
    case UIState::WIFI_SYNC:          drawSyncScreen(renderer, gpio); break;
    case UIState::SCREEN_EDITOR:      drawScreenEditor(renderer, gpio); break;
    case UIState::VC_BROWSER:         drawVcBrowser(renderer, gpio); break;
    default: break;
  }
}

// See screen_editor.h for why the interpreter needs this: a running BASIC
// program blocks loopTask, so PRINT output would otherwise sit in the
// grid buffer, invisible, until the program finished or broke out.
//
// The wait loops below MUST yield (vTaskDelay, not just pollRefresh() in
// a tight spin) -- confirmed on hardware: without it, this function alone
// busy-loops for the ~635-650ms an e-ink refresh takes, back-to-back,
// starving the FreeRTOS idle task just as badly as mb_run() itself did
// before the stepped-handler fix (same "Task watchdog ... IDLE (CPU 0)"
// spam, just moved here instead of gone).
static void waitForRefresh() {
  while (renderer.isRefreshing()) {
    renderer.pollRefresh();
    vTaskDelay(1);
  }
}

void screenEditorFlushDisplay() {
  unsigned long t0 = millis();
  waitForRefresh();
  unsigned long t1 = millis();
  drawScreenEditor(renderer, gpio);
  unsigned long t2 = millis();
  waitForRefresh();
  unsigned long t3 = millis();
  screenDirty = false;
  DBG_PRINTF("[FLUSH] waitPrev=%lums draw=%lums waitNew=%lums total=%lums\n",
             t1 - t0, t2 - t1, t3 - t2, t3 - t0);
}

void setup() {
  DBG_INIT();
  DBG_PRINTLN("MicroBASIC starting...");

  setCpuFrequencyMhz(80);

  gpio.begin();
  display.begin();

  renderer.setFadingFix(true);  // Power down display analog circuits after each refresh — reduces idle drain
  rendererSetup(renderer);

  // Load persisted UI settings from NVS early so startup screen uses saved orientation
  uiPrefs.begin("ui_prefs", false);
  currentOrientation = static_cast<Orientation>(uiPrefs.getUChar("orient", 0));
  darkMode = uiPrefs.getBool("darkMode", false);
  writingMode = static_cast<WritingMode>(uiPrefs.getUChar("writeMode", 0));
  fontSize = static_cast<FontSize>(uiPrefs.getUChar("fontSize", 2));
  showWordCount = uiPrefs.getBool("showWC", true);
  remapButtonsInPrograms = uiPrefs.getBool("btnRemap", false);

  // Apply saved orientation
  applyOrientationToRenderer(currentOrientation);

  editorInit();
  inputSetup();
  fileManagerSetup();
  sdDateTimeSetup();  // data dos arquivos no SD -- ver sd_datetime.h

  // Migrate the settings/backup dir before anything reads or writes it
  // (BLE pairing, WiFi credentials, UI prefs below all live under it).
  ensureSettingsDir();

  // Restore UI prefs from SD backup if NVS was wiped by a firmware flash
  if (!uiPrefs.isKey("orient")) {
    static char uiBuf[128];
    if (sdReadFile("/MicroBASIC/ui_prefs.json", uiBuf, sizeof(uiBuf))) {
      int o  = jsonGetInt(uiBuf, "orient");
      int d  = jsonGetInt(uiBuf, "dark");
      int wm = jsonGetInt(uiBuf, "writeMode");
      int fs = jsonGetInt(uiBuf, "fontSize");
      int wc = jsonGetInt(uiBuf, "showWC");
      int br = jsonGetInt(uiBuf, "btnRemap");
      if (o  >= 0) { uiPrefs.putUChar("orient",    (uint8_t)o);  currentOrientation = static_cast<Orientation>(o); }
      if (d  >= 0) { uiPrefs.putBool("darkMode",   d != 0);      darkMode           = (d != 0); }
      if (wm >= 0) { uiPrefs.putUChar("writeMode", (uint8_t)wm); writingMode        = static_cast<WritingMode>(wm); }
      if (fs >= 0) { uiPrefs.putUChar("fontSize",  (uint8_t)fs); fontSize           = static_cast<FontSize>(fs); }
      if (wc >= 0) { uiPrefs.putBool("showWC",     wc != 0);     showWordCount      = (wc != 0); }
      if (br >= 0) { uiPrefs.putBool("btnRemap",   br != 0);     remapButtonsInPrograms = (br != 0); }
      // Re-apply orientation in case it changed
      applyOrientationToRenderer(currentOrientation);
      DBG_PRINTLN("UI prefs restored from SD backup");
    }
  }

  // Boots straight into SCREEN_EDITOR (currentState's initial value) --
  // force its landscape-CCW override now, same as entering it from the
  // menu would, without touching the just-loaded currentOrientation.
  screenEditorReset();
  applyOrientationToRenderer(Orientation::LANDSCAPE_CCW);
  // Segurar BACK no arranque pula o autoexec.bas. Sem essa saida, um autoexec
  // com defeito exige cabo e esptool para recuperar -- que e exatamente o
  // buraco em que este projeto caiu ao ganhar autoexec sem ganhar como sair
  // dele. As maquinas da epoca tinham a mesma tecla de escape.
  //
  // Le o estado CRU e exige que ele persista: a primeira conversao do ADC
  // depois do boot devolve 0, que classifica como RIGHT, e uma leitura
  // espuria nao pode decidir isto sozinha.
  {
    int held = 0;
    for (int i = 0; i < 12; i++) {
      if (gpio.rawPressed(HalGPIO::BTN_BACK)) held++;
      delay(25);
    }
    if (held >= 10) tbSetAutoexecEnabled(false);
  }
  tbSetup();  // TinyBasic: direct-mode statements (see tb_bridge.h)

  bleSetup();

  // Enable automatic light sleep between loop iterations.
  // CONFIG_PM_ENABLE and CONFIG_FREERTOS_USE_TICKLESS_IDLE are compiled into
  // ESP-IDF via sdkconfig.defaults (framework = arduino, espidf). BLE modem
  // sleep keeps the radio alive across sleep/wake cycles.
  esp_pm_config_esp32c3_t pm_config = {
    .max_freq_mhz = 80,
    .min_freq_mhz = 10,
    .light_sleep_enable = true
  };
  esp_err_t pm_err = esp_pm_configure(&pm_config);
  DBG_PRINTF("PM configure: %s\n", esp_err_to_name(pm_err));

  // Initialize auto-reconnect to enabled by default
  autoReconnectEnabled = true;

  // Register this app's name in shared NVS and detect other OTA apps. The
  // dual-boot sibling (CPR-vCodex, the reader) shows this name in its own
  // switcher menu entry — this firmware is built on MicroWriter (itself
  // credited to MicroSlate in NOTICE.md), but the product name here is
  // "MicroBASIC", used without an "X4" hardware suffix so this identifier
  // (like the mDNS hostname below) stays correct if this codebase is ever
  // ported to a different device.
  registerOtaAppName("MicroBASIC");
  detectOtaApps();

  DBG_PRINTLN("MicroBASIC ready.");

  // The display needs one FULL_REFRESH after power-on to initialize its analog
  // circuits before FAST_REFRESH will work.
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);

  screenDirty = true;
}

// Enter deep sleep - matches crosspoint pattern
void enterDeepSleep(SleepReason reason) {
  DBG_PRINTLN("Entering deep sleep...");
  
  // Render the sleep screen before entering deep sleep
  renderSleepScreen();

  // Save any unsaved work
  if (currentState == UIState::TEXT_EDITOR && editorHasUnsavedChanges()) {
    saveCurrentFile();
  }

  display.deepSleep();     // Power down display first
  gpio.startDeepSleep();   // Waits for power button release, then sleeps
  // Will not return - device is asleep
}

// Translate physical button presses to HID key codes
// NOTE: gpio.update() is called in loop() before this function
// Called from the runtime's byield() while a BASIC program runs. loop() is
// not running then -- the program *is* what loop() is doing -- so without
// this the physical d-pad is dead for the whole run. That matters most at
// boot, where an autoexec launcher is exactly the moment no keyboard is
// paired yet, and it also means the games work without one.
//
// Deliberately NOT processPhysicalButtons(): that owns sleep, the power
// button and UI state transitions, and firing those from inside the
// interpreter would pull the ground out from under the running program.
// This only enqueues navigation keys, which the program reads through GET
// like any other key.
// Depois que um programa BASIC termina, os botoes tem de ser vistos soltos
// antes de gerarem borda de novo. Sem isso, um unico toque fisico dispara
// DUAS vezes: uma no pump abaixo (que roda enquanto o programa executa) e
// outra em processPhysicalButtons(), cujo estado de borda ficou congelado em
// "solto" durante todo o RUN. Foi assim que o ENTER que escolhia uma opcao
// do lancador reaparecia no editor de tela, executando a linha que o
// programa tinha acabado de imprimir -- "Syntax Error" logo abaixo dela.
static bool physRearmPending = false;

void physicalButtonsRearm() { physRearmPending = true; }

void pumpPhysicalButtonsForProgram() {
  static bool lu = false, ld = false, ll = false, lr = false, lc = false, lb = false;
  gpio.update();

  const bool u = gpio.isPressed(HalGPIO::BTN_UP);
  const bool d = gpio.isPressed(HalGPIO::BTN_DOWN);
  const bool l = gpio.isPressed(HalGPIO::BTN_LEFT);
  const bool r = gpio.isPressed(HalGPIO::BTN_RIGHT);
  const bool c = gpio.isPressed(HalGPIO::BTN_CONFIRM);
  const bool b = gpio.isPressed(HalGPIO::BTN_BACK);

  // The power button, and it is not optional. While a program runs, loop()
  // is blocked inside the interpreter, so processPhysicalButtons() -- which
  // normally owns this -- never runs. A program that loops forever waiting
  // for a key (a game, a menu) therefore left the device with NO way to
  // switch off: holding power did nothing, because nothing was reading it.
  //
  // Safe to call from in here precisely because enterDeepSleep() does not
  // return. There is no half-torn-down interpreter to worry about.
  // O power so passa a contar depois de ter sido visto solto uma vez. Ligar
  // o aparelho e segurar o power -- e se um programa ja estiver rodando
  // quando o loop chega aqui, o contador de 3s comecava a correr com o dedo
  // ainda no botao. Ligar mandava dormir. Mesma logica da guarda do d-pad,
  // que eu tinha escrito e nao apliquei aqui.
  static bool powerSeenUp = false;
  static bool powerHeldHere = false;
  static unsigned long powerStartHere = 0;
  const bool pw = gpio.isPressed(HalGPIO::BTN_POWER);
  if (!powerSeenUp) {
    if (!pw) powerSeenUp = true;
  } else if (pw && !powerHeldHere) {
    powerHeldHere = true;
    powerStartHere = millis();
  } else if (!pw) {
    powerHeldHere = false;
  } else if (millis() - powerStartHere > 3000) {
    enterDeepSleep(SleepReason::POWER_LONGPRESS);
  }

  auto fire = [](uint8_t k) {
    enqueueKeyEvent(k, 0, true);
    enqueueKeyEvent(k, 0, false);
  };

  // Com o remap ligado, cada botao manda a seta que ele realmente aponta na
  // tela em paisagem, medido no aparelho: direita->cima, cima->esquerda,
  // baixo->direita, esquerda->baixo. Sem ele, 1:1 com o nome do botao.
  const uint8_t kUp    = remapButtonsInPrograms ? HID_KEY_LEFT  : HID_KEY_UP;
  const uint8_t kDown  = remapButtonsInPrograms ? HID_KEY_RIGHT : HID_KEY_DOWN;
  const uint8_t kLeft  = remapButtonsInPrograms ? HID_KEY_DOWN  : HID_KEY_LEFT;
  const uint8_t kRight = remapButtonsInPrograms ? HID_KEY_UP    : HID_KEY_RIGHT;

  if (u && !lu) fire(kUp);
  if (d && !ld) fire(kDown);
  if (l && !ll) fire(kLeft);
  if (r && !lr) fire(kRight);
  if (c && !lc) fire(HID_KEY_ENTER);
  // Physical Back breaks a running program, the same gesture Escape gives a
  // BLE keyboard. Without it, the only way out of a program with no exit was
  // a keyboard -- see inputConsumeBreakPending(), which looks for exactly
  // this key.
  if (b && !lb) fire(HID_KEY_ESCAPE);

  lu = u; ld = d; ll = l; lr = r; lc = c; lb = b;
}

static void processPhysicalButtons() {
  static bool btnUpLast = false;
  static bool btnDownLast = false;
  static bool btnLeftLast = false;
  static bool btnRightLast = false;
  static bool btnConfirmLast = false;
  static bool btnBackLast = false;

  // Use isPressed() — persistent debounced state.  With one-shot scanning
  // (radio quiet during navigation), InputManager debounce works reliably.
  //
  // The phantom RIGHT that used to plague this -- a press with nobody
  // touching the device, which in SCREEN_EDITOR silently corrupted whatever
  // BASIC line was being typed -- is fixed in InputManager: it reports
  // nothing until the raw ADC reading has once said "nothing pressed".
  //
  // Measured, not guessed. The first two conversions after boot return
  // exactly 0 on the button channel, and 0 falls in RIGHT's band, which has
  // no floor. Flooring the band was the obvious fix and would have been
  // wrong: a real RIGHT press also reads 0, so the discriminator has to be
  // *when*, not *what*. Two earlier mitigations (more debounce; suppressing
  // reads after BLE connect) were tried and reverted. See
  // docs/DEVELOPMENT_LOG.md for the full account.
  bool btnUp      = gpio.isPressed(HalGPIO::BTN_UP);
  bool btnDown    = gpio.isPressed(HalGPIO::BTN_DOWN);
  bool btnLeft    = gpio.isPressed(HalGPIO::BTN_LEFT);
  bool btnRight   = gpio.isPressed(HalGPIO::BTN_RIGHT);
  bool btnConfirm = gpio.isPressed(HalGPIO::BTN_CONFIRM);
  bool btnBack    = gpio.isPressed(HalGPIO::BTN_BACK);

  // Rearme apos um programa BASIC -- ver physicalButtonsRearm() acima.
  if (physRearmPending) {
    if (btnUp || btnDown || btnLeft || btnRight || btnConfirm || btnBack) {
      btnUp = btnDown = btnLeft = btnRight = btnConfirm = btnBack = false;
    } else {
      physRearmPending = false;
    }
  }

  // Power button state machine for proper long/short press handling
  static bool powerHeld = false;
  static unsigned long powerPressStart = 0;
  static bool sleepTriggered = false;

  bool btnPower = gpio.isPressed(HalGPIO::BTN_POWER);

  if (btnPower && !powerHeld) {
    // Button just pressed
    powerHeld = true;
    sleepTriggered = false;
    powerPressStart = millis();
  }

  if (btnPower && powerHeld && !sleepTriggered) {
    if (millis() - powerPressStart > 3000) {
      sleepTriggered = true;
      enterDeepSleep(SleepReason::POWER_LONGPRESS);
      return; // Exit early to prevent further processing
    }
  }

  if (!btnPower && powerHeld) {
    // Button released
    unsigned long duration = millis() - powerPressStart;
    powerHeld = false;

    if (!sleepTriggered && duration > 50 && duration < 1000) {
      // Short press - go to main menu (except when already there)
      if (currentState != UIState::MAIN_MENU) {
        if (currentState == UIState::TEXT_EDITOR && editorHasUnsavedChanges()) {
          saveCurrentFile();
        }
        currentState = UIState::MAIN_MENU;
        screenDirty = true;
      }
    }
  }

  // Back button long-press for restart
  static bool backHeld = false;
  static unsigned long backPressStart = 0;
  static bool restartTriggered = false;

  if (btnBack && !backHeld) {
    backHeld = true;
    restartTriggered = false;
    backPressStart = millis();
  }

  if (btnBack && backHeld && !restartTriggered) {
    if (millis() - backPressStart > 5000) {
      restartTriggered = true;
      DBG_PRINTLN("BACK held for 5s — restarting device...");
      if (currentState == UIState::TEXT_EDITOR && editorHasUnsavedChanges()) {
        saveCurrentFile();
      }
      delay(100);
      ESP.restart();
    }
  }

  if (!btnBack && backHeld) {
    backHeld = false;
  }

  // Map physical buttons to HID key codes based on current UI state
  switch (currentState) {
    case UIState::MAIN_MENU:
      if ((btnUp && !btnUpLast) || (btnLeft && !btnLeftLast)) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if ((btnDown && !btnDownLast) || (btnRight && !btnRightLast)) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      break;

    case UIState::FILE_BROWSER:
      if (((btnUp && !btnUpLast) || (btnLeft && !btnLeftLast)) && getFileCount() > 0) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if (((btnDown && !btnDownLast) || (btnRight && !btnRightLast)) && getFileCount() > 0) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnConfirm && !btnConfirmLast && getFileCount() > 0) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::TEXT_EDITOR: {
      // Key repeat state for held navigation/backspace keys
      static uint8_t repeatKey = 0;
      static unsigned long repeatStart = 0;
      static unsigned long lastRepeat = 0;
      const unsigned long REPEAT_DELAY = 400;
      const unsigned long REPEAT_RATE  = 80;

      auto fireKey = [](uint8_t k) {
        enqueueKeyEvent(k, 0, true);
        enqueueKeyEvent(k, 0, false);
      };

      // Map currently held button to HID key (0 = none)
      uint8_t heldKey = 0;
      if      (btnUp)    heldKey = HID_KEY_UP;
      else if (btnDown)  heldKey = HID_KEY_DOWN;
      else if (btnLeft)  heldKey = HID_KEY_LEFT;
      else if (btnRight) heldKey = HID_KEY_RIGHT;

      if (heldKey != repeatKey) {
        // Key changed — fire immediately on press
        if (heldKey != 0) fireKey(heldKey);
        repeatKey   = heldKey;
        repeatStart = millis();
        lastRepeat  = millis();
      } else if (heldKey != 0) {
        unsigned long now = millis();
        if (now - repeatStart > REPEAT_DELAY && now - lastRepeat > REPEAT_RATE) {
          fireKey(heldKey);
          lastRepeat = now;
        }
      }

      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        if (editorHasUnsavedChanges()) saveCurrentFile();
        currentState = UIState::FILE_BROWSER;
        screenDirty = true;
      }
      break;
    }

    case UIState::SCREEN_EDITOR: {
      // Cursor repeat, same feel as TEXT_EDITOR's held-navigation handling.
      static uint8_t repeatKey = 0;
      static unsigned long repeatStart = 0;
      static unsigned long lastRepeat = 0;
      const unsigned long REPEAT_DELAY = 400;
      const unsigned long REPEAT_RATE = 80;

      auto fireKey = [](uint8_t k) {
        enqueueKeyEvent(k, 0, true);
        enqueueKeyEvent(k, 0, false);
      };

      // Physical RIGHT used to be left out of this list: it sits at the
      // bottom of the shared-ADC ladder and was firing on its own, silently
      // moving the cursor mid-line while a program was being typed. That was
      // a mitigation, and the cause has since been found and fixed --
      // InputManager now reports nothing until the raw reading has been seen
      // at rest once, which is what the phantom was. Restored.
      uint8_t heldKey = 0;
      if      (btnUp)    heldKey = HID_KEY_UP;
      else if (btnDown)  heldKey = HID_KEY_DOWN;
      else if (btnLeft)  heldKey = HID_KEY_LEFT;
      else if (btnRight) heldKey = HID_KEY_RIGHT;

      if (heldKey != repeatKey) {
        if (heldKey != 0) {
          DBG_PRINTF("[PHYSBTN] SCREEN_EDITOR nav fired: key=0x%02X (btnUp=%d btnDown=%d btnLeft=%d btnRight=%d)\n",
                     heldKey, btnUp, btnDown, btnLeft, btnRight);
          fireKey(heldKey);
        }
        repeatKey = heldKey;
        repeatStart = millis();
        lastRepeat = millis();
      } else if (heldKey != 0) {
        unsigned long now = millis();
        if (now - repeatStart > REPEAT_DELAY && now - lastRepeat > REPEAT_RATE) {
          fireKey(heldKey);
          lastRepeat = now;
        }
      }

      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;
    }

    // VC drives entirely from the arrow keys, so it maps the same way as
    // the menus: physical d-pad to navigation, Confirm to Enter, Back to
    // Escape. RIGHT is deliberately included here (unlike SCREEN_EDITOR,
    // where the phantom-press issue made it unusable) -- a stray press here
    // only moves a selection, it can't corrupt anything typed.
    case UIState::VC_BROWSER:
      if (btnUp && !btnUpLast) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if (btnDown && !btnDownLast) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnLeft && !btnLeftLast) {
        enqueueKeyEvent(HID_KEY_LEFT, 0, true);
        enqueueKeyEvent(HID_KEY_LEFT, 0, false);
      }
      if (btnRight && !btnRightLast) {
        enqueueKeyEvent(HID_KEY_RIGHT, 0, true);
        enqueueKeyEvent(HID_KEY_RIGHT, 0, false);
      }
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::RENAME_FILE:
    case UIState::NEW_FILE:
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::BLUETOOTH_SETTINGS:
      if (btnUp && !btnUpLast) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if (btnDown && !btnDownLast) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnRight && !btnRightLast) {
        enqueueKeyEvent(HID_KEY_RIGHT, 0, true);  // Scan
        enqueueKeyEvent(HID_KEY_RIGHT, 0, false);
      }
      if (btnLeft && !btnLeftLast) {
        enqueueKeyEvent(HID_KEY_LEFT, 0, true);   // Disconnect
        enqueueKeyEvent(HID_KEY_LEFT, 0, false);
      }
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::PAIRED_KEYBOARDS:
      if ((btnUp && !btnUpLast) || (btnLeft && !btnLeftLast)) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if ((btnDown && !btnDownLast) || (btnRight && !btnRightLast)) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::WIFI_SYNC:
      if ((btnUp && !btnUpLast) || (btnLeft && !btnLeftLast)) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if ((btnDown && !btnDownLast) || (btnRight && !btnRightLast)) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::SETTINGS:
      if ((btnUp && !btnUpLast) || (btnLeft && !btnLeftLast)) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if ((btnDown && !btnDownLast) || (btnRight && !btnRightLast)) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    default:
      break;
  }

  // Update last state
  btnUpLast = btnUp;
  btnDownLast = btnDown;
  btnLeftLast = btnLeft;
  btnRightLast = btnRight;
  btnConfirmLast = btnConfirm;
  btnBackLast = btnBack;
}

// Global variable for activity tracking
static unsigned long lastActivityTime = 0;
// Sleep after this long with no input. Five minutes was the original value
// and is short for a device you sit and think in front of.
const unsigned long IDLE_TIMEOUT = 15UL * 60UL * 1000UL; // 15 minutes

void registerActivity() {
  lastActivityTime = millis();
}

// Function to render the sleep screen
void renderSleepScreen() {
  renderer.clearScreen();
  
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  
  // Title: "MicroBASIC"
  const char* title = "MicroBASIC";
  int titleWidth = renderer.getTextAdvanceX(FONT_BODY, title);
  int titleX = (sw - titleWidth) / 2;
  int titleY = sh * 0.35; // 35% down the screen (moved up)
  renderer.drawText(FONT_BODY, titleX, titleY, title, true, EpdFontFamily::BOLD);
  
  // Subtitle: "Asleep"
  const char* subtitle = "Asleep";
  int subTitleWidth = renderer.getTextAdvanceX(FONT_UI, subtitle);
  int subTitleX = (sw - subTitleWidth) / 2;
  int subTitleY = sh * 0.48; // 48% down the screen (moved up)
  renderer.drawText(FONT_UI, subTitleX, subTitleY, subtitle, true);
  
  // Footer: "Hold Power to wake"
  const char* footer = "Hold Power to wake";
  int footerWidth = renderer.getTextAdvanceX(FONT_SMALL, footer);
  int footerX = (sw - footerWidth) / 2;
  int footerY = sh * 0.75; // 75% down the screen (moved up from bottom)
  renderer.drawText(FONT_SMALL, footerX, footerY, footer);
  
  // Perform a full display refresh to ensure the sleep screen is visible
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  
  // Small delay to ensure the display update is complete
  delay(500);
}

void loop() {
  // --- GPIO first: always poll buttons before anything else ---
  gpio.update();

  // Control auto-reconnect based on UI state
  static UIState lastState = UIState::MAIN_MENU;
  if (currentState == UIState::BLUETOOTH_SETTINGS) {
    autoReconnectEnabled = false;
    // On first entry to BT settings, do a one-shot scan
    if (lastState != UIState::BLUETOOTH_SETTINGS) {
      cancelPendingConnection();
      startDeviceScan();  // One-shot 5s scan, radio goes quiet after
    }
  } else {
    autoReconnectEnabled = true;
    if (lastState == UIState::BLUETOOTH_SETTINGS && isDeviceScanning()) {
      stopDeviceScan();
    }
  }
  lastState = currentState;

  // Process BLE (connection handling, scan completion detection)
  bleLoop();

  // Process WiFi sync HTTP clients when active
  if (isWifiSyncActive()) wifiSyncLoop();

  // CRITICAL: Process buttons BEFORE checking wasAnyPressed() to avoid consuming button states
  processPhysicalButtons();
  int inputEventsProcessed = processAllInput(); // Assuming this returns number of events processed

  // Register activity AFTER button processing (don't consume button states prematurely)
  static unsigned long lastInputTime = 0;
  bool hadActivity = gpio.wasAnyPressed() || inputEventsProcessed > 0;
  if (hadActivity) {
    registerActivity();
    lastInputTime = millis();
  }

  // Auto-save: hybrid idle + hard cap for crash protection.
  // - Saves after 10s of no keystrokes (catches natural pauses between sentences)
  // - Hard cap every 2min during continuous typing (never lose more than 2min of work)
  static unsigned long lastAutoSaveMs = 0;
  if (currentState == UIState::TEXT_EDITOR
      && editorHasUnsavedChanges()
      && editorGetCurrentFile()[0] != '\0') {
    unsigned long now = millis();
    bool idleTrigger = (now - lastInputTime) > AUTO_SAVE_IDLE_MS
                    && (now - lastAutoSaveMs) > AUTO_SAVE_IDLE_MS;
    bool capTrigger  = (now - lastAutoSaveMs) > AUTO_SAVE_MAX_MS;
    if (idleTrigger || capTrigger) {
      lastAutoSaveMs = now;
      saveCurrentFile(false);  // Skip refreshFileList — file list unchanged by content update
    }
  }

  // Periodically refresh sync screen to show status changes (every 2s)
  if (currentState == UIState::WIFI_SYNC) {
    static unsigned long lastSyncRefresh = 0;
    if (millis() - lastSyncRefresh > 2000) {
      screenDirty = true;
      lastSyncRefresh = millis();
    }
  }

  // Poll display refresh — non-blocking check of BUSY pin
  if (renderer.isRefreshing()) {
    renderer.pollRefresh();
  }

  // Don't start a new screen update while display is still refreshing
  if (screenDirty && !renderer.isRefreshing()) {
    updateScreen();
  }

  // O autoexec so roda depois que o loop deu uma volta inteira e a tela foi
  // desenhada uma vez. Rodando no setup(), como fazia antes, o loop() nunca
  // comecava: sem power, sem desenho, sem saida. Aqui, se o programa nunca
  // terminar, pelo menos a interface ja existiu e o power ja passou pelo
  // pump de botoes.
  {
    static bool autoexecTried = false;
    if (!autoexecTried) {
      autoexecTried = true;
      tbRunPendingAutoexec();
    }
  }

  // Persist UI settings to NVS when they change (NVS write only on change, not every loop)
  static Orientation lastSavedOrientation = currentOrientation;
  static bool lastSavedDarkMode = darkMode;
  static WritingMode lastSavedWritingMode = writingMode;
  static FontSize lastSavedFontSize = fontSize;
  static bool lastSavedShowWordCount = showWordCount;
  static bool lastSavedRemapButtons = remapButtonsInPrograms;
  if (currentOrientation != lastSavedOrientation || darkMode != lastSavedDarkMode
      || writingMode != lastSavedWritingMode || fontSize != lastSavedFontSize
      || showWordCount != lastSavedShowWordCount
      || remapButtonsInPrograms != lastSavedRemapButtons) {
    uiPrefs.putUChar("orient", static_cast<uint8_t>(currentOrientation));
    uiPrefs.putBool("darkMode", darkMode);
    uiPrefs.putUChar("writeMode", static_cast<uint8_t>(writingMode));
    uiPrefs.putUChar("fontSize", static_cast<uint8_t>(fontSize));
    uiPrefs.putBool("showWC", showWordCount);
    uiPrefs.putBool("btnRemap", remapButtonsInPrograms);
    lastSavedOrientation = currentOrientation;
    lastSavedDarkMode = darkMode;
    lastSavedWritingMode = writingMode;
    lastSavedFontSize = fontSize;
    lastSavedShowWordCount = showWordCount;
    lastSavedRemapButtons = remapButtonsInPrograms;
    // Keep SD backup in sync so settings survive a firmware flash
    static char uiBuf[128];
    snprintf(uiBuf, sizeof(uiBuf),
             "{\"orient\":%d,\"dark\":%d,\"writeMode\":%d,\"fontSize\":%d,"
             "\"showWC\":%d,\"btnRemap\":%d}",
             (int)currentOrientation, darkMode ? 1 : 0,
             (int)writingMode, (int)fontSize, showWordCount ? 1 : 0,
             remapButtonsInPrograms ? 1 : 0);
    ensureSettingsDir();
    sdWriteFile("/MicroBASIC/ui_prefs.json", uiBuf);
  }

  // Check for idle timeout (skip while WiFi sync is active)
  if (!isWifiSyncActive() && millis() - lastActivityTime > IDLE_TIMEOUT) {
    enterDeepSleep(SleepReason::IDLE_TIMEOUT);
  }

  // Adaptive delay with recently-active window for button responsiveness.
  // BLE keystrokes wake from light sleep via modem interrupt (delay value irrelevant).
  // Physical buttons are polled, so the idle delay must be short enough to catch a
  // quick tap (~80-150ms). 50ms idle guarantees 1-2 samples per press.
  // Stay at fast polling for 2s after any activity for snappy consecutive presses.
  static constexpr unsigned long ACTIVE_WINDOW_MS = 2000;
  bool recentlyActive = (millis() - lastInputTime) < ACTIVE_WINDOW_MS;
  delay((hadActivity || screenDirty || recentlyActive) ? 10 : 50);
}
