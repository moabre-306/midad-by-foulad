#include "TrmnlSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "TrmnlSyncActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "trmnl/TrmnlClient.h"
#include "trmnl/TrmnlStore.h"

namespace fui = freeink::ui;

namespace {
enum MenuItem : int16_t { SERVER_URL, LINK, DEVICE_MAC, FLIP, SHOW_NOW, UNLINK, MENU_ITEMS };
}  // namespace

void TrmnlSettingsActivity::onEnter() {
  UiListActivity::onEnter();
  RenderLock lock(*this);
  rebuildRows();
}

void TrmnlSettingsActivity::rebuildRows() {
  rowItems.clear();
  rowValues.clear();
  rowItems.reserve(MENU_ITEMS);
  rowValues.reserve(MENU_ITEMS);

  const char* arrow = I18N.isRtl() ? "<" : ">";
  auto addRow = [this](const StrId label, const MenuItem id, std::string value) {
    fui::ListItem item;
    item.label = I18N.get(label);
    item.actionValue = id;
    rowValues.push_back(std::move(value));
    item.value = rowValues.back().c_str();
    rowItems.push_back(item);
  };

  addRow(StrId::STR_TRMNL_SERVER, SERVER_URL,
         TRMNL_STORE.serverUrl.empty() ? std::string(tr(STR_DEFAULT_VALUE)) : TRMNL_STORE.serverUrl);
  std::string linkValue = tr(STR_TRMNL_NOT_LINKED);
  if (TRMNL_STORE.isLinked()) {
    linkValue = TRMNL_STORE.friendlyId.empty() ? std::string(tr(STR_TRMNL_LINKED)) : TRMNL_STORE.friendlyId;
  }
  addRow(StrId::STR_TRMNL_LINK, LINK, std::move(linkValue));
  addRow(StrId::STR_TRMNL_DEVICE_ID, DEVICE_MAC, TrmnlClient::macAddress());

  fui::ListItem flip;
  flip.label = I18N.get(StrId::STR_TRMNL_FLIP);
  flip.actionValue = FLIP;
  flip.toggle = true;
  flip.toggleChecked = TRMNL_STORE.flipScreen;
  rowValues.emplace_back();
  rowItems.push_back(flip);

  addRow(StrId::STR_TRMNL_SHOW_NOW, SHOW_NOW, arrow);
  addRow(StrId::STR_TRMNL_UNLINK, UNLINK, "");
}

// Same unlocked rebuild as TouchSettingsGroupActivity's activation/result paths.
void TrmnlSettingsActivity::refreshRows() {
  rebuildRows();
  requestUpdate();
}

void TrmnlSettingsActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  nav.selected = index;
  const auto id = static_cast<MenuItem>(rowItems[static_cast<size_t>(index)].actionValue);

  switch (id) {
    case SERVER_URL: {
      app.clearTapFlash();
      startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_TRMNL_SERVER),
                                                                     TRMNL_STORE.getBaseUrl(), 128, InputType::Url),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& kb = std::get<KeyboardResult>(result.data);
                                 std::string url = (kb.text == "https://" || kb.text == "http://") ? "" : kb.text;
                                 while (!url.empty() && url.back() == '/') url.pop_back();
                                 if (url == TrmnlStore::DEFAULT_SERVER_URL) url.clear();
                                 if (url != TRMNL_STORE.serverUrl) {
                                   // A token from one server is not valid on another.
                                   TRMNL_STORE.serverUrl = url;
                                   TRMNL_STORE.unlink();
                                   TRMNL_STORE.saveToFile();
                                 }
                               }
                               refreshRows();
                             });
      break;
    }
    case LINK:
    case SHOW_NOW:
      app.clearTapFlash();
      startActivityForResult(
          std::make_unique<TrmnlSyncActivity>(
              renderer, mappedInput, id == LINK ? TrmnlSyncActivity::Mode::LINK : TrmnlSyncActivity::Mode::SHOW),
          [this](const ActivityResult&) { refreshRows(); });
      break;
    case FLIP:
      TRMNL_STORE.flipScreen = !TRMNL_STORE.flipScreen;
      TRMNL_STORE.saveToFile();
      refreshRows();
      break;
    case UNLINK:
      TRMNL_STORE.unlink();
      TRMNL_STORE.saveToFile();
      refreshRows();
      break;
    case DEVICE_MAC:
    default:
      break;
  }
}

const char* TrmnlSettingsActivity::headerTitle() const { return tr(STR_TRMNL); }

void TrmnlSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}
