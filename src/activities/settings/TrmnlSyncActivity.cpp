#include "TrmnlSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "trmnl/TrmnlScreen.h"
#include "trmnl/TrmnlStore.h"

void TrmnlSyncActivity::onEnter() {
  Activity::onEnter();

  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }
  startActivityForResult(
      std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
      [this](const ActivityResult& wifiResult) { onWifiSelectionComplete(!wifiResult.isCancelled); });
}

void TrmnlSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    {
      RenderLock lock(*this);
      state = FAILED;
      result = TrmnlClient::Result::NoWifi;
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = WORKING;
  }
  requestUpdate();

  TrmnlClient::Result outcome;
  if (mode == Mode::LINK) {
    outcome = TrmnlClient::linkDevice();
  } else {
    uint32_t nextRefreshSeconds = 0;
    outcome = TrmnlClient::refreshScreen(nextRefreshSeconds);
  }

  {
    RenderLock lock(*this);
    result = outcome;
    const bool ok = outcome == TrmnlClient::Result::Ok || outcome == TrmnlClient::Result::Unchanged;
    if (!ok) {
      state = FAILED;
    } else {
      state = (mode == Mode::SHOW && TrmnlClient::hasScreen()) ? SHOWING : SUCCESS;
    }
  }
  requestUpdate();
}

void TrmnlSyncActivity::onExit() {
  Activity::onExit();

  // Like the other Wi-Fi flows: power the radio down and reboot onto a clean heap.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestartToSettings();
  }
}

void TrmnlSyncActivity::render(RenderLock&&) {
  if (state == SHOWING && TrmnlScreen::draw(renderer)) return;

  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_TRMNL));
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - lineHeight) / 2;

  if (state == WIFI_SELECTION || state == WORKING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, mode == Mode::LINK ? tr(STR_TRMNL_LINKING) : tr(STR_TRMNL_FETCHING));
  } else if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_TRMNL_LINKED), true, EpdFontFamily::BOLD);
    if (!TRMNL_STORE.friendlyId.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + 10, TRMNL_STORE.friendlyId.c_str());
    }
    renderer.drawCenteredText(SMALL_FONT_ID, top + 2 * (lineHeight + 10), tr(STR_TRMNL_SLEEP_HINT));
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, top,
                              mode == Mode::LINK ? tr(STR_TRMNL_LINK_FAILED) : tr(STR_TRMNL_FETCH_FAILED), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + 10, I18N.get(TrmnlClient::resultMessage(result)));
    if (result == TrmnlClient::Result::NotRegistered) {
      renderer.drawCenteredText(UI_10_FONT_ID, top + 2 * (lineHeight + 10), TrmnlClient::macAddress().c_str());
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void TrmnlSyncActivity::loop() {
  if (state != SUCCESS && state != SHOWING && state != FAILED) return;
  int x = 0;
  int y = 0;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
      mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Down) || mappedInput.wasScreenTapped(x, y)) {
    finish();
  }
}
