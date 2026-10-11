#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"

/**
 * Settings -> TRMNL: server URL, device linking, and a test fetch. The TRMNL
 * screen itself is shown by the TRMNL sleep screen mode.
 */
class TrmnlSettingsActivity final : public UiListActivity {
 public:
  explicit TrmnlSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("TrmnlSettings", renderer, mappedInput) {}

  void onEnter() override;

 private:
  std::vector<freeink::ui::ListItem> rowItems;
  // Backing storage for rowItems[].value; reserved up front so the pointers stay valid.
  std::vector<std::string> rowValues;

  void rebuildRows();
  void refreshRows();

  int listCount() const override { return static_cast<int>(rowItems.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
};
