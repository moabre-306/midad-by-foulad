#include "SettingsActionDispatch.h"

#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <memory>

#include "CrossPointSettings.h"
#include "FouladEbooksConfig.h"
#include "OpdsServerStore.h"
#include "SilentRestart.h"
#include "activities/Activity.h"
#include "activities/apps/DictionaryActivity.h"
#include "activities/browser/FouladLogoutActivity.h"
#include "activities/browser/FouladQrLoginActivity.h"
#include "activities/home/FileBrowserActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/settings/ButtonRemapActivity.h"
#include "activities/settings/ClearCacheActivity.h"
#include "activities/settings/FontDownloadActivity.h"
#include "activities/settings/KOReaderSettingsActivity.h"
#include "activities/settings/LanguageSelectActivity.h"
#include "activities/settings/OpdsServerListActivity.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "activities/settings/StatusBarSettingsActivity.h"
#include "activities/util/ConfirmationActivity.h"

void dispatchSettingAction(Activity& host, GfxRenderer& renderer, MappedInputManager& mappedInput,
                           const SettingAction action, const std::function<void()>& onRebuildNeeded) {
  auto resultHandler = [](const ActivityResult&) { SETTINGS.saveToFile(); };

  switch (action) {
    case SettingAction::RemapFrontButtons:
      host.startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput), resultHandler);
      break;
    case SettingAction::CustomiseStatusBar:
      host.startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput), resultHandler);
      break;
    case SettingAction::Dictionary:
      host.startActivityForResult(std::make_unique<DictionaryActivity>(renderer, mappedInput), resultHandler);
      break;
    case SettingAction::KOReaderSync:
      host.startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput), resultHandler);
      break;
    case SettingAction::OPDSBrowser:
      host.startActivityForResult(std::make_unique<OpdsServerListActivity>(renderer, mappedInput), resultHandler);
      break;
    case SettingAction::Network:
      // Settings -> Wi-Fi Networks is the ONE WiFi surface with no parent
      // flow and no exit reboot: WifiSelectionActivity deliberately leaves
      // the connection to its caller, and every other caller reboots (which
      // powers the modem off). Coming back here, fully power the radio down
      // or it idles in STA mode (~20-30 mA) until the next deep sleep --
      // confirmed as the fast-battery-drain path (user report). Saved
      // credentials auto-reconnect the next time a flow needs WiFi.
      host.startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false),
                                  [onRebuildNeeded](const ActivityResult&) {
                                    if (WiFi.getMode() != WIFI_MODE_NULL) {
                                      WiFi.disconnect(true);
                                      WiFi.mode(WIFI_OFF);
                                      LOG_INF("SETTINGS", "WiFi radio powered off after network screen");
                                    }
                                    SETTINGS.saveToFile();
                                    onRebuildNeeded();
                                  });
      break;
    case SettingAction::ClearCache:
      host.startActivityForResult(std::make_unique<ClearCacheActivity>(renderer, mappedInput), resultHandler);
      break;
    case SettingAction::CheckForUpdates:
      // Reboot into the OTA flow instead of opening it in this session: after
      // Home/library browsing the heap is fragmented below what the GitHub TLS
      // handshakes need (see silentRestartToOtaCheck). Does not return.
      silentRestartToOtaCheck();
      break;
    case SettingAction::SdFirmwareUpdate:
      host.startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput), resultHandler);
      break;
    case SettingAction::DownloadFonts:
      host.startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput),
                                  [onRebuildNeeded](const ActivityResult&) {
                                    SETTINGS.saveToFile();
                                    onRebuildNeeded();
                                  });
      break;
    case SettingAction::Language:
      host.startActivityForResult(std::make_unique<LanguageSelectActivity>(renderer, mappedInput), resultHandler);
      break;
    case SettingAction::BrowseFiles:
      host.startActivityForResult(std::make_unique<FileBrowserActivity>(renderer, mappedInput), resultHandler);
      break;
    case SettingAction::FileTransfer:
      // Reboot into the web server on a fresh heap (see SilentRestart.h):
      // starting it from a long-running session leaves the WiFi driver and
      // TCP buffers fighting a fragmented heap and page loads crawl. The
      // activity's own exit path already silentRestart()s back to Home.
      silentRestartToFileTransfer();
      break;
    case SettingAction::FouladEbooksLogout: {
      // Confirm, then reboot into the sign-out. The credential is only cleared
      // once the server confirms removal, so the device never believes it is
      // signed out while the account still lists it.
      //
      // The reboot is not cosmetic. The
      // self-revoke POST is a TLS handshake, and from Settings with WiFi already
      // up it found a 9,716-byte largest block against the ~17KB a handshake
      // needs -- it could not open the connection at all. Same treatment OTA,
      // File Transfer and QR sign-in already get.
      auto logoutHandler = [onRebuildNeeded](const ActivityResult& result) {
        if (result.isCancelled) {
          onRebuildNeeded();
          return;
        }
        silentRestartToLogout();  // does not return
      };
      host.startActivityForResult(
          std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_FOULAD_EBOOKS_LOGOUT_CONFIRM), ""),
          logoutHandler);
      break;
    }
    case SettingAction::FouladEbooksLogin:
      // Same QR sign-in as the first-run home entry. On success it stores the
      // issued token and silent-reboots into the catalog on a fresh heap; on
      // cancel we return here, so rebuild the list either way to flip this row
      // between Login and Logout.
      host.startActivityForResult(std::make_unique<FouladQrLoginActivity>(renderer, mappedInput),
                                  [onRebuildNeeded](const ActivityResult&) { onRebuildNeeded(); });
      break;
    case SettingAction::None:
      // Do nothing
      break;
  }
}
