#pragma once

#include "activities/Activity.h"
#include "trmnl/TrmnlClient.h"

/**
 * Connects to Wi-Fi, then links this device to the TRMNL server (LINK) or
 * fetches and shows the current TRMNL screen (SHOW).
 */
class TrmnlSyncActivity final : public Activity {
 public:
  enum class Mode { LINK, SHOW };

  explicit TrmnlSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Mode mode)
      : Activity("TrmnlSync", renderer, mappedInput), mode(mode) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == WORKING; }
  const char* activityDebugName() const override { return "TrmnlSyncActivity"; }

 private:
  enum State { WIFI_SELECTION, WORKING, SUCCESS, SHOWING, FAILED };

  Mode mode;
  State state = WIFI_SELECTION;
  TrmnlClient::Result result = TrmnlClient::Result::Ok;

  void onWifiSelectionComplete(bool success);
};
