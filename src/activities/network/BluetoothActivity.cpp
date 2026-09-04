#include "BluetoothActivity.h"

#ifndef SIMULATOR
// No simulator-side counterpart exists for this HAL component (see main.cpp's own
// guarded include of the same header) -- every access below is guarded so this whole
// translation unit still compiles and renders in the simulator with a placeholder
// state, per BLE-R2's simulator-compilability goal (unlike the stale branch's design,
// which excluded this file from the simulator build entirely).
#include <BlePeripheralManager.h>

#include "BleWifiAutoConnectCache.h"
#include "BleWifiScanCache.h"
#endif
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <WiFi.h>

#include "I18n.h"
#include "MappedInputManager.h"
#include "components/icons/bluetooth64.h"
#include "fontIds.h"
#include "reading/ReadingStatsStore.h"

#ifndef SIMULATOR
// Combined WiFi line for the two BLE-driven radio state machines. Nullptr when
// neither is doing anything, so the screen stays clean in the normal case.
const char* BluetoothActivity::wifiStatusText() {
  switch (BleWifiScanCache::currentState()) {
    case BleWifiScanCache::State::PendingStart:
    case BleWifiScanCache::State::Scanning:
      return tr(STR_BLUETOOTH_WIFI_SCANNING);
    case BleWifiScanCache::State::Failed:
      return tr(STR_BLUETOOTH_WIFI_FAILED);
    default:
      break;
  }
  switch (BleWifiAutoConnectCache::currentState()) {
    case BleWifiAutoConnectCache::State::PendingStart:
    case BleWifiAutoConnectCache::State::Connecting:
      return tr(STR_BLUETOOTH_WIFI_CONNECTING);
    case BleWifiAutoConnectCache::State::Done:
      return tr(STR_BLUETOOTH_WIFI_CONNECTED);
    case BleWifiAutoConnectCache::State::Failed:
      return tr(STR_BLUETOOTH_WIFI_FAILED);
    default:
      break;
  }
  return nullptr;
}

// One value covering both machines, so loop() can notice a change and repaint.
int BluetoothActivity::wifiStateKey() {
  return static_cast<int>(BleWifiScanCache::currentState()) * 16 +
         static_cast<int>(BleWifiAutoConnectCache::currentState());
}
#endif

BluetoothActivity::UiState BluetoothActivity::currentUiState() const {
#ifndef SIMULATOR
  switch (BlePeripheral.state()) {
    case BlePeripheralManager::State::Off:
      return UiState::Starting;
    case BlePeripheralManager::State::Advertising:
      return UiState::Advertising;
    case BlePeripheralManager::State::Connected:
      return UiState::Connected;
    case BlePeripheralManager::State::PausedLowMemory:
      return UiState::PausedLowMemory;
  }
  return UiState::Starting;
#else
  // Simulator placeholder: always reads as "about to advertise" here, since there is
  // no real radio to poll -- purely for visually validating layout/RTL, not for
  // exercising actual BLE behavior (which the simulator can never do regardless, see
  // CLAUDE.md's simulator-testing notes).
  return UiState::Advertising;
#endif
}

void BluetoothActivity::onEnter() {
  Activity::onEnter();
  lastRenderedUiState_ = -1;

  // Aggressively release everything reclaimable that this simple a page has no use
  // for, before requesting the radio -- both proven safe to call cross-activity
  // (global singletons, not owned by whichever activity happened to fill them):
  // OtaUpdateActivity.cpp already does the same pair for the same reason (needing
  // every spare block before a memory-heavy operation).
  READING_STATS.releaseMemory();
  if (auto* fcm = renderer.getFontCacheManager()) fcm->clearCache();

#ifndef SIMULATOR
  // BLE and WiFi are mutually exclusive here: main.cpp's bleAllowedNow gate requires
  // WiFi.getMode() == WIFI_MODE_NULL, so a radio left in STA mode by an earlier flow
  // means begin() is never called and this screen sits on "Starting Bluetooth..."
  // forever. Nothing logged it either, because the gate only reports on a TRANSITION
  // and it never transitioned.
  //
  // Reproduced on the X3 by pairing, signing out and re-pairing (all of which leave
  // WiFi up) and then opening this screen. Neither entry point -- the Apps tile nor
  // Home's hold-Confirm shortcut -- powered the radio down, and only the Settings
  // Wi-Fi screen ever did.
  //
  // This activity owns BLE's lifetime, so it owns the precondition too. Saved
  // credentials auto-reconnect whenever a later flow needs WiFi.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);  // credentials are kept: eraseap defaults false
    WiFi.mode(WIFI_OFF);
    poweredWifiDown_ = true;
    LOG_INF("BLE", "WiFi powered off so the radio can start");
  }

  BlePeripheral.setUserRequested(true);
#endif

  requestUpdate();
}

void BluetoothActivity::onExit() {
#ifndef SIMULATOR
  // Mirror of onEnter()'s request -- this is the ONLY place BLE gets released, so
  // every exit path (Back, or navigating elsewhere) must go through here. Radio
  // teardown itself happens via main.cpp's normal bleAllowedNow lifecycle on the
  // next tick, same as it always has.
  BlePeripheral.setUserRequested(false);

  // Put the radio back the way we found it. Without this, leaving this screen
  // left WiFi off with no indication, and the next screen that wanted the
  // network looked simply broken -- reported from the device as "wifi didn't
  // connect" after using BLE. Saved credentials are intact (disconnect(true)
  // does not erase them), so begin() reconnects to the last network.
  if (poweredWifiDown_) {
    poweredWifiDown_ = false;

    // BLE must be torn down SYNCHRONOUSLY first. setUserRequested(false) above
    // only takes effect when main.cpp's bleAllowedNow gate re-evaluates on its
    // NEXT loop iteration, so without this the WiFi calls below run with NimBLE
    // still fully resident (~57KB). On an X3 that left the driver unable to get
    // its buffers and it dereferenced the null it got back:
    //
    //   E wifi: Expected to init 4 rx buffer, actual is 1
    //   Guru Meditation Error: Core 0 panic'ed (Load access fault)
    //   MCAUSE 0x5  MTVAL 0x0  A0 0x0   -- free heap 25,488, largest block 18,420
    //
    // BleWifiScanCache and BleWifiAutoConnectCache already do exactly this, for
    // exactly this reason; this path was added later and missed it. end() is a
    // full synchronous NimBLE teardown and a no-op when already off.
    if (BlePeripheral.isActive()) {
      BlePeripheral.end();
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin();
    LOG_INF("BLE", "WiFi restored on leaving the Bluetooth screen");
  }
#endif
  Activity::onExit();
}

void BluetoothActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

#ifndef SIMULATOR
  // The radio machines advance on the main loop independently of BLE's own state,
  // so nothing else would repaint this screen while a scan or connect runs.
  const int wifiKey = wifiStateKey();
  if (wifiKey != lastRenderedWifiState_) {
    lastRenderedWifiState_ = wifiKey;
    requestUpdate();
  }
#endif

  const int nowState = static_cast<int>(currentUiState());
  if (nowState != lastRenderedUiState_) {
    requestUpdate();
  }
}

void BluetoothActivity::render(RenderLock&&) {
  const UiState state = currentUiState();
  lastRenderedUiState_ = static_cast<int>(state);
#ifndef SIMULATOR
  lastRenderedWifiState_ = wifiStateKey();
#endif

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  const int iconSize = 64;
  const int iconX = (pageWidth - iconSize) / 2;
  const int iconY = pageHeight / 2 - iconSize - 40;
  renderer.drawIcon(Bluetooth64Icon, iconX, iconY, iconSize);

  const int titleY = iconY + iconSize + 20;
  renderer.drawCenteredText(UI_12_FONT_ID, titleY, tr(STR_MIDAD_BLE), true, EpdFontFamily::BOLD);

  // Device name: static regardless of state (a pure eFuse read, needs no radio up) --
  // shown from the moment this screen opens, so the user can cross-check it against
  // a scanner even while the radio is still starting up.
  int nextY = titleY + renderer.getLineHeight(UI_12_FONT_ID) + 8;
  char advName[24] = {};
#ifndef SIMULATOR
  BlePeripheralManager::getAdvertisedName(advName, sizeof(advName));
#else
  snprintf(advName, sizeof(advName), "Midad-XXXXXX");
#endif
  renderer.drawCenteredText(SMALL_FONT_ID, nextY, advName, true);
  nextY += renderer.getLineHeight(SMALL_FONT_ID) + 12;

  const char* statusText = tr(STR_BLUETOOTH_STARTING);
  const char* hintText = nullptr;
  switch (state) {
    case UiState::Starting:
      statusText = tr(STR_BLUETOOTH_STARTING);
      break;
    case UiState::Advertising:
      statusText = tr(STR_BLUETOOTH_READY);
      hintText = tr(STR_BLUETOOTH_OPEN_APP_HINT);
      break;
    case UiState::Connected:
      statusText = tr(STR_CONNECTED);
      break;
    case UiState::PausedLowMemory:
      statusText = tr(STR_BLUETOOTH_PAUSED_LOW_MEMORY);
      break;
  }
  renderer.drawCenteredText(UI_12_FONT_ID, nextY, statusText, true, EpdFontFamily::BOLD);
  nextY += renderer.getLineHeight(UI_12_FONT_ID) + 8;

#ifndef SIMULATOR
  // What the PHONE just asked this device to do. Without it the reader shows only
  // its own BLE state, so a wifi.scan or wifi.autoconnect issued from the app is
  // invisible here -- the screen sits on "Ready" while the radio is actually busy,
  // and a failed connect looks identical to nothing having happened.
  //
  // It also explains the handover: a successful connect releases BLE (see
  // BleWifiAutoConnectCache::finishConnect), so this line is what tells the user
  // the Bluetooth link ended because WiFi took the radio, rather than because
  // something broke.
  const char* wifiText = wifiStatusText();
  if (wifiText) {
    renderer.drawCenteredText(SMALL_FONT_ID, nextY, wifiText, true);
    nextY += renderer.getLineHeight(SMALL_FONT_ID) + 8;
  }
#endif

  if (hintText) {
    renderer.drawCenteredText(SMALL_FONT_ID, nextY, hintText, true);
  }

  // Back is the only way out now -- and the only way BLE ever stops -- so it's the
  // only hint left at the bottom (see this class's own header comment).
  const int exitHintY = pageHeight - renderer.getLineHeight(SMALL_FONT_ID) - 20;
  renderer.drawCenteredText(SMALL_FONT_ID, exitHintY, tr(STR_BLUETOOTH_EXIT_HINT), true);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
