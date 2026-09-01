#include "WifiSelectionActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HeapTrace.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "WifiConnectionHintStore.h"
#include "WifiCredentialStore.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/WifiDiagLog.h"

#ifndef SIMULATOR
namespace {

// STA event counters. Written only by the Arduino network-event task, read only by the
// activity's loop() task: monotonic, 32-bit and naturally aligned, so a reader sees the
// old or the new value and never a torn one. A mutex would have to be taken inside the
// event callback to protect nothing.
//
// A plain function pointer, not a lambda: WiFi.onEvent()'s std::function overload
// heap-allocates a closure per registration (see CLAUDE.md on std::function), and
// nothing here needs to capture. File-static rather than per-activity for the same
// reason the callback cannot capture -- and because a counter that outlives the
// activity cannot be left dangling by one.
//
// They exist because ESP-IDF's events carry no application-level attempt id. Snapshotting
// a monotonic counter at each attempt boundary supplies the generation the event itself
// does not: a GOT_IP that did not advance the counter past an attempt's start belongs to
// an earlier attempt.
volatile uint32_t staGotIpEvents = 0;
volatile uint32_t staDisconnectEvents = 0;

void onStaNetworkEvent(arduino_event_id_t event) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    ++staGotIpEvents;
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    ++staDisconnectEvents;
  }
}

}  // namespace
#endif

void WifiSelectionActivity::onEnter() {
  Activity::onEnter();

  // Load saved WiFi credentials - SD card operations need lock as we use SPI
  // for both
  {
    RenderLock lock(*this);
    WIFI_STORE.loadFromFile();
    WIFI_HINTS.loadFromFile();
  }

#ifndef SIMULATOR
  // Registered for the whole activity, removed in onExit(): these supply the observable
  // disconnect boundary the hint fallback waits on, and the generation stamp completion
  // is validated against.
  WiFi.onEvent(onStaNetworkEvent, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(onStaNetworkEvent, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
#endif

  // Reset state
  selectedNetworkIndex = 0;
  networks.clear();
  realNetworkCount = 0;
  state = WifiSelectionState::SCANNING;
  selectedSSID.clear();
  connectedIP.clear();
  connectionError.clear();
  enteredPassword.clear();
  usedSavedPassword = false;
  savePromptSelection = 0;
  forgetPromptSelection = 0;
  autoConnecting = false;
  manualNetworkListRequested = false;
  attemptChannel = 0;
  hintFallbackDone = false;
  awaitingTimeSync = false;
  hintCancelPending = false;
  lateCompletionLogged = false;
  associationMs = 0;
  autoAttemptedSsids.clear();
  autoAttemptedSsids.reserve(WIFI_STORE.getCredentialCount());

  // Cache MAC address for display
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macStr[64];
  snprintf(macStr, sizeof(macStr), "%s %02x-%02x-%02x-%02x-%02x-%02x", tr(STR_MAC_ADDRESS), mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
  cachedMacAddress = std::string(macStr);

  // No requestUpdate() here: it only sets a flag that ActivityManager::loop() acts on
  // after onEnter() returns, so the paint landed *after* whichever radio path below ran,
  // not before it. startWifiScan() paints its own screen ahead of its blocking bring-up;
  // attemptConnection() requests one that lands as soon as the asynchronous
  // WiFi.begin() has been issued.

  // Attempt to auto-connect to known networks. Try the last successful network
  // first for speed (no scan needed to know it's worth trying); if that's not
  // saved or fails, an auto-scan (started below / by handleAutoConnectFailure)
  // tries every other saved network the scan finds, by signal strength. The
  // user can interrupt this at any point and drop straight to the manual list.
  if (allowAutoConnect && WIFI_STORE.getCredentialCount() > 0) {
    const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
    if (!lastSsid.empty()) {
      const auto cred = WIFI_STORE.findCredential(lastSsid);
      if (cred && tryAutoConnectCredential(*cred)) {
        return;
      }
    }

    startWifiScan(true);
    return;
  }

  // Fallback to scanning
  startWifiScan();
}

void WifiSelectionActivity::onExit() {
  Activity::onExit();

#ifndef SIMULATOR
  // Before anything else: the callback must not outlive the activity that armed it.
  WiFi.removeEvent(onStaNetworkEvent, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.removeEvent(onStaNetworkEvent, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  HEAP_TRACE_EVENT("TEMP:WIFI.exit", "mode=%d status=%d completions=%u attempts=%lu", static_cast<int>(WiFi.getMode()),
                   static_cast<int>(WiFi.status()), static_cast<unsigned>(completionCount),
                   static_cast<unsigned long>(attemptId));
#endif

  LOG_DBG("WIFI", "Free heap at onExit start: %d bytes", ESP.getFreeHeap());

  // Stop any ongoing WiFi scan
  LOG_DBG("WIFI", "Deleting WiFi scan...");
  WiFi.scanDelete();
  LOG_DBG("WIFI", "Free heap after scanDelete: %d bytes", ESP.getFreeHeap());

  // Note: We do NOT disconnect WiFi here - the parent activity
  // (CrossPointWebServerActivity) manages WiFi connection state. We just clean
  // up the scan and task.

  LOG_DBG("WIFI", "Free heap at onExit end: %d bytes", ESP.getFreeHeap());
}

void WifiSelectionActivity::startWifiScan(const bool autoScan) {
  // TEMP diagnostic: brackets WiFi stack bring-up (radio init + async scan start),
  // which is the first large allocator on the network path.
  HEAP_TRACE_SCOPE("WIFI.start");
  autoConnecting = autoScan;
  manualNetworkListRequested = false;
  state = WifiSelectionState::SCANNING;
  networks.clear();
  // Blocking paint, not requestUpdate(): everything below stalls the calling task for
  // roughly a second (WiFi stack bring-up on a cold radio, plus the explicit delay), and
  // a deferred update wouldn't reach the panel until after that, leaving the previous
  // screen up with no sign the scan had started.
  requestUpdateAndWait();

  // Set WiFi mode to station
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  // Start async scan
  WiFi.scanNetworks(true);  // true = async scan
}

void WifiSelectionActivity::processWifiScanResults() {
  const int16_t scanResult = WiFi.scanComplete();

  if (scanResult == WIFI_SCAN_RUNNING) {
    // Scan still in progress
    return;
  }

  if (scanResult == WIFI_SCAN_FAILED) {
    networks.clear();
    realNetworkCount = 0;
    appendHiddenNetworkEntry();
    autoConnecting = false;
    state = WifiSelectionState::NETWORK_LIST;
    selectedNetworkIndex = 0;
    requestUpdate();
    return;
  }

  // Scan complete, process results — deduplicate in-place, keeping strongest signal
  networks.clear();
  networks.reserve(scanResult);

  for (int i = 0; i < scanResult; i++) {
    char ssid[33];
    strlcpy(ssid, WiFi.SSID(i).c_str(), sizeof(ssid));
    const int32_t rssi = WiFi.RSSI(i);

    // Skip hidden networks (empty SSID)
    if (ssid[0] == '\0') {
      continue;
    }

    auto it =
        std::find_if(networks.begin(), networks.end(), [&ssid](const WifiNetworkInfo& n) { return n.ssid == ssid; });
    if (it == networks.end()) {
      WifiNetworkInfo network;
      network.ssid = ssid;
      network.rssi = rssi;
      network.isEncrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      network.hasSavedPassword = WIFI_STORE.hasSavedCredential(network.ssid);
      networks.push_back(std::move(network));
    } else if (rssi > it->rssi) {
      it->rssi = rssi;
      it->isEncrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
  }

  // Sort: saved-password networks first, then by signal strength (strongest first)
  std::sort(networks.begin(), networks.end(), [](const WifiNetworkInfo& a, const WifiNetworkInfo& b) {
    if (a.hasSavedPassword != b.hasSavedPassword) {
      return a.hasSavedPassword;
    }
    return a.rssi > b.rssi;
  });

  realNetworkCount = networks.size();
  appendHiddenNetworkEntry();

  WiFi.scanDelete();

  if (autoConnecting && !manualNetworkListRequested && tryNextSavedNetworkFromScan()) {
    return;
  }

  autoConnecting = false;
  state = WifiSelectionState::NETWORK_LIST;
  selectedNetworkIndex = 0;
  requestUpdate();
}

void WifiSelectionActivity::appendHiddenNetworkEntry() {
  // Synthetic list entry that lets the user type an SSID that is not broadcast.
  // ESP32 can join hidden APs as long as the SSID is supplied to WiFi.begin().
  WifiNetworkInfo placeholder;
  placeholder.rssi = 0;
  placeholder.isEncrypted = true;  // Treated as encrypted; an empty password still connects open APs
  placeholder.hasSavedPassword = false;
  placeholder.isHiddenPlaceholder = true;
  networks.push_back(std::move(placeholder));
}

void WifiSelectionActivity::selectNetwork(const int index) {
  if (index < 0 || index >= static_cast<int>(networks.size())) {
    return;
  }

  const auto& network = networks[index];

  // Synthetic "Add hidden network..." entry: prompt the user to type the SSID first
  if (network.isHiddenPlaceholder) {
    promptHiddenSsid();
    return;
  }

  selectedSSID = network.ssid;
  selectedRequiresPassword = network.isEncrypted;
  usedSavedPassword = false;
  enteredPassword.clear();
  autoConnecting = false;

  // Check if we have saved credentials for this network
  const auto savedCred = WIFI_STORE.findCredential(selectedSSID);
  if (savedCred && !savedCred->password.empty()) {
    // Use saved password - connect directly
    enteredPassword = savedCred->password;
    usedSavedPassword = true;
    LOG_DBG("WiFi", "Using saved credentials for %s", selectedSSID.c_str());
    attemptConnection();
    return;
  }

  if (selectedRequiresPassword) {
    promptPasswordEntry();
  } else {
    // Connect directly for open networks
    attemptConnection();
  }
}

void WifiSelectionActivity::promptPasswordEntry() {
  // Show password entry
  state = WifiSelectionState::PASSWORD_ENTRY;
  // Don't allow screen updates while changing activity
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_ENTER_WIFI_PASSWORD),
                                                                 "",  // No initial text
                                                                 64,  // Max password length
                                                                 InputType::Password),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             state = WifiSelectionState::NETWORK_LIST;
                           } else {
                             enteredPassword = std::get<KeyboardResult>(result.data).text;
                             // state will be updated in next loop iteration
                           }
                         });
}

void WifiSelectionActivity::promptHiddenSsid() {
  selectedSSID.clear();
  selectedRequiresPassword = true;  // Hidden networks are usually encrypted; empty password still joins open APs
  usedSavedPassword = false;
  enteredPassword.clear();
  autoConnecting = false;

  // Suppress rendering during the activity transition (see render()).
  state = WifiSelectionState::HIDDEN_SSID_ENTRY;
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_ENTER_WIFI_SSID),
                                                                 "",  // No initial text
                                                                 32,  // Max SSID length (IEEE 802.11: 32 bytes)
                                                                 InputType::Text),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             state = WifiSelectionState::NETWORK_LIST;
                             return;
                           }
                           selectedSSID = std::get<KeyboardResult>(result.data).text;
                           if (selectedSSID.empty()) {
                             state = WifiSelectionState::NETWORK_LIST;
                           }
                           // Otherwise stay in HIDDEN_SSID_ENTRY; loop() continues the flow.
                         });
}

bool WifiSelectionActivity::hasAttemptedAutoSsid(const std::string& ssid) const {
  return std::find(autoAttemptedSsids.begin(), autoAttemptedSsids.end(), ssid) != autoAttemptedSsids.end();
}

bool WifiSelectionActivity::tryAutoConnectCredential(const WifiCredential& cred) {
  if (hasAttemptedAutoSsid(cred.ssid)) {
    return false;
  }

  LOG_DBG("WIFI", "Attempting saved network: %s", cred.ssid.c_str());
  autoAttemptedSsids.push_back(cred.ssid);
  selectedSSID = cred.ssid;
  enteredPassword = cred.password;
  selectedRequiresPassword = !cred.password.empty();
  usedSavedPassword = true;
  autoConnecting = true;
  manualNetworkListRequested = false;
  attemptConnection();  // paints "Connecting to <ssid>" itself, before the radio work
  return true;
}

bool WifiSelectionActivity::tryNextSavedNetworkFromScan() {
  for (const auto& network : networks) {
    if (!network.hasSavedPassword || hasAttemptedAutoSsid(network.ssid)) {
      continue;
    }

    const auto cred = WIFI_STORE.findCredential(network.ssid);
    if (cred && tryAutoConnectCredential(*cred)) {
      return true;
    }
  }
  return false;
}

void WifiSelectionActivity::handleAutoConnectFailure() {
  LOG_DBG("WIFI", "Saved network failed: %s", selectedSSID.c_str());
  WiFi.disconnect();

  if (!networks.empty()) {
    if (tryNextSavedNetworkFromScan()) {
      return;
    }
    WifiDiagLog::append("auto-connect exhausted all saved networks in scan, falling back to manual list");
    autoConnecting = false;
    state = WifiSelectionState::NETWORK_LIST;
    selectedNetworkIndex = 0;
    requestUpdate();
    return;
  }

  startWifiScan(true);
}

void WifiSelectionActivity::showNetworkListFromAutoConnect() {
  LOG_DBG("WIFI", "User requested manual network list");
  WiFi.disconnect();
  autoConnecting = false;
  manualNetworkListRequested = true;

  if (networks.empty()) {
    startWifiScan(false);
    manualNetworkListRequested = true;
    return;
  }

  state = WifiSelectionState::NETWORK_LIST;
  selectedNetworkIndex = 0;
  requestUpdate();
}

void WifiSelectionActivity::attemptConnection() {
  // TEMP diagnostic: brackets WiFi.begin() only. Association completes
  // asynchronously; the paired "WIFI.connected" mark below is the real end edge.
  HEAP_TRACE_SCOPE("WIFI.connect");
  state = autoConnecting ? WifiSelectionState::AUTO_CONNECTING : WifiSelectionState::CONNECTING;
  connectedIP.clear();
  connectionError.clear();
  hintFallbackDone = false;
  // Cleared here rather than only where cancellation completes: the user can leave an
  // in-flight cancellation via Confirm (showNetworkListFromAutoConnect) or Back, and a
  // flag left set would make the next attempt cancel itself on its first tick.
  hintCancelPending = false;
  WifiDiagLog::append("connecting: ssid=" + selectedSSID + " auto=" + (autoConnecting ? "1" : "0") +
                      " savedPassword=" + (usedSavedPassword ? "1" : "0"));
  // requestUpdate(), not requestUpdateAndWait(): the panel paints as soon as
  // ActivityManager::loop() regains control, which is right after the asynchronous
  // WiFi.begin() below has been issued -- association then runs for seconds afterwards,
  // so "Connecting to <ssid>" is still up for effectively all of it. Waiting here put a
  // full e-ink repaint, plus the SD append above, in front of the radio work.
  requestUpdate();

  prepareRadio();
  beginConnection(/*useHint=*/true);
}

void WifiSelectionActivity::prepareRadio() {
  WiFi.persistent(false);  // Credentials are managed by WifiCredentialStore; suppress SDK NVS auto-connect
  WiFi.mode(WIFI_STA);
  // eraseap, but not wifioff: aborting an in-progress SDK auto-connect and clearing the
  // NVS-saved SSID is the point here, whereas also powering the radio down made the
  // following WiFi.begin() pay a full driver bring-up on every single attempt. The
  // settle delay stays because esp_wifi_disconnect() completes asynchronously and an
  // esp_wifi_connect() issued inside that window is rejected with ESP_ERR_WIFI_CONN.
  WiFi.disconnect(false, true);
  delay(100);

  // Scan all channels so networks with multiple APs use the strongest matching
  // BSSID instead of the first match found by the framework's default fast scan.
  // Only reached when no BSSID hint applies: a pinned WiFi.begin() skips scanning.
  // Guarded out of the simulator build: crosspoint-simulator's WiFiClass stub
  // doesn't implement these ESP-IDF-specific scan-tuning calls.
#ifndef SIMULATOR
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
#endif

  // Set hostname so routers show "CrossPoint-Reader-AABBCCDDEEFF" instead of "esp32-XXXXXXXXXXXX"
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String hostname = "CrossPoint-Reader-" + mac;
  WiFi.setHostname(hostname.c_str());
}

void WifiSelectionActivity::beginConnection(const bool useHint) {
  std::memset(attemptBssid, 0, sizeof(attemptBssid));
  attemptChannel = 0;

#ifndef SIMULATOR
  if (useHint) {
    // Where this SSID was last actually joined. Pinning WiFi.begin() to it removes the
    // full-channel scan the framework would otherwise run before every association.
    const auto hint = WIFI_HINTS.find(selectedSSID);
    if (hint) {
      std::memcpy(attemptBssid, hint->bssid, sizeof(attemptBssid));
      attemptChannel = hint->channel;
    }
  }
#else
  (void)useHint;
#endif

  const char* password = (selectedRequiresPassword && !enteredPassword.empty()) ? enteredPassword.c_str() : nullptr;

  // New generation for this attempt, with the GOT_IP counter snapshotted against it: a
  // WL_CONNECTED that does not advance that counter past this point was produced by an
  // earlier attempt (see completionBelongsToActiveAttempt).
  ++attemptId;
  lateCompletionLogged = false;
#ifndef SIMULATOR
  gotIpAtAttemptStart = staGotIpEvents;
#endif

  // Last statement before WiFi.begin(): every timeout in wifi_connect_policy is a budget
  // for the radio, so display and SD work must not be charged against it.
  connectionStartTime = millis();
  HEAP_TRACE_EVENT("TEMP:WIFI.begin", "attempt=%lu mode=%s ch=%u atMs=%lu", static_cast<unsigned long>(attemptId),
                   attemptChannel != 0 ? "hinted" : "fullscan", static_cast<unsigned>(attemptChannel),
                   connectionStartTime);
#ifndef SIMULATOR
  if (attemptChannel != 0) {
    WiFi.begin(selectedSSID.c_str(), password, attemptChannel, attemptBssid, true);
    return;
  }
#endif
  WiFi.begin(selectedSSID.c_str(), password);
}

void WifiSelectionActivity::requestHintCancellation(const bool hardFail) {
  hintCancelPending = true;
  hintFallbackDone = true;
  cancelledAttemptId = attemptId;
  hintCancelStartMs = millis();
#ifndef SIMULATOR
  disconnectsAtCancel = staDisconnectEvents;
#endif
  HEAP_TRACE_EVENT("TEMP:WIFI.cancelReq", "attempt=%lu reason=%s afterMs=%lu", static_cast<unsigned long>(attemptId),
                   hardFail ? "failed" : "timeout", millis() - connectionStartTime);
  WiFi.disconnect(false, false);
}

WifiSelectionActivity::CancelProgress WifiSelectionActivity::advanceHintCancellation() {
#ifdef SIMULATOR
  return CancelProgress::Confirmed;
#else
  const unsigned long waitedMs = millis() - hintCancelStartMs;

  if (staDisconnectEvents != disconnectsAtCancel) {
    // The superseded attempt is definitively finished, so any later GOT_IP can only
    // belong to whatever is started next.
    HEAP_TRACE_EVENT("TEMP:WIFI.cancelOk", "attempt=%lu via=event waitedMs=%lu status=%d",
                     static_cast<unsigned long>(cancelledAttemptId), waitedMs, static_cast<int>(WiFi.status()));
    return CancelProgress::Confirmed;
  }

  if (waitedMs < wifi_connect_policy::HINT_CANCEL_CONFIRM_TIMEOUT_MS) {
    return CancelProgress::Waiting;
  }

  // Silence is not confirmation. With no disconnect event the superseded attempt may
  // still be alive, and issuing WiFi.begin() here would put two attempts in flight --
  // the exact race this confirmation exists to prevent, and one the event counters
  // cannot untangle afterwards, because a GOT_IP landing after the new attempt's
  // snapshot is indistinguishable from that attempt's own. So escalate to a stop that
  // can be observed, rather than waiting a second time and hoping.
  HEAP_TRACE_EVENT("TEMP:WIFI.cancelTimeout", "attempt=%lu waitedMs=%lu status=%d",
                   static_cast<unsigned long>(cancelledAttemptId), waitedMs, static_cast<int>(WiFi.status()));

  // WiFi.mode(WIFI_OFF) runs esp_wifi_stop() and then esp_wifi_deinit() inline and
  // reports whether they succeeded; getMode() then reads WIFI_MODE_NULL back out of the
  // driver's own started flag. Both are synchronous, so this is an observed stopped
  // state and not another timeout -- and a stopped, deinitialised driver has no station
  // left to deliver a callback for the old attempt.
  const bool stopped = WiFi.mode(WIFI_OFF) && WiFi.getMode() == WIFI_MODE_NULL;
  HEAP_TRACE_EVENT("TEMP:WIFI.radioStop", "attempt=%lu stopped=%d mode=%d",
                   static_cast<unsigned long>(cancelledAttemptId), stopped ? 1 : 0, static_cast<int>(WiFi.getMode()));
  if (!stopped) {
    // A definite negative, not an expired clock: the radio will not say it stopped, so
    // there is no point at which starting another attempt would be sound.
    return CancelProgress::Unrecoverable;
  }
  return CancelProgress::ConfirmedAfterRadioStop;
#endif
}

bool WifiSelectionActivity::completionBelongsToActiveAttempt() {
#ifdef SIMULATOR
  return true;
#else
  // A GOT_IP newer than this attempt's start is the only thing that makes a
  // WL_CONNECTED this attempt's own. Withheld rather than failed: the counter is bumped
  // by the same event dispatch that sets WL_CONNECTED, so a genuine completion resolves
  // on the very next tick, while a superseded one never advances the counter and is
  // left to the ordinary connection timeout.
  if (staGotIpEvents != gotIpAtAttemptStart) {
    return true;
  }
  if (!lateCompletionLogged) {
    lateCompletionLogged = true;
    HEAP_TRACE_EVENT("TEMP:WIFI.lateReject", "activeAttempt=%lu expiredAttempt=%lu gotIp=%lu atStart=%lu",
                     static_cast<unsigned long>(attemptId), static_cast<unsigned long>(cancelledAttemptId),
                     static_cast<unsigned long>(staGotIpEvents), static_cast<unsigned long>(gotIpAtAttemptStart));
  }
  return false;
#endif
}

void WifiSelectionActivity::rememberConnectionHint() {
#ifndef SIMULATOR
  const int channel = WiFi.channel();
  if (!wifi_connect_policy::isValidChannel(channel)) return;

  uint8_t bssid[wifi_connect_policy::BSSID_LEN] = {};
  WiFi.BSSID(bssid);
  WIFI_HINTS.remember(selectedSSID, bssid, static_cast<uint8_t>(channel));
#endif
}

void WifiSelectionActivity::checkConnectionStatus() {
  if (state != WifiSelectionState::CONNECTING && state != WifiSelectionState::AUTO_CONNECTING) {
    return;
  }

  const wl_status_t status = WiFi.status();

  // A superseded hinted attempt is still being torn down: nothing may complete, and the
  // replacement attempt may not start, until that teardown has actually been observed.
  if (hintCancelPending) {
    if (status == WL_CONNECTED) {
      // The expired attempt associated after being told to stop. It cannot be the
      // replacement -- that has not been issued yet -- so this is attributable, and
      // acting on it would complete the activity on an attempt already declared dead.
      if (!lateCompletionLogged) {
        lateCompletionLogged = true;
        HEAP_TRACE_EVENT("TEMP:WIFI.lateReject", "activeAttempt=none expiredAttempt=%lu status=%d waitedMs=%lu",
                         static_cast<unsigned long>(cancelledAttemptId), static_cast<int>(status),
                         millis() - hintCancelStartMs);
      }
    }
    const CancelProgress progress = advanceHintCancellation();
    if (progress == CancelProgress::Waiting) {
      return;
    }
    hintCancelPending = false;

    // The hinted attempt is finished either way, so the hint that produced it goes.
    {
      RenderLock lock(*this);
      WIFI_HINTS.forget(selectedSSID);
    }

    if (progress == CancelProgress::Unrecoverable) {
      // Nothing here may start another attempt. Fail this connection through the
      // ordinary failure path instead: auto-connect moves on to the next saved network
      // (or the manual list), and a user-initiated connect gets the usual failure
      // screen, from which Confirm or Back offers a retry. Reuses the existing timeout
      // string rather than adding one: the attempt did time out, and inventing an
      // untranslated message for a rare device fault would be worse than reusing an
      // accurate one.
      connectionError = tr(STR_ERROR_CONNECTION_TIMEOUT);
      WifiDiagLog::append("hint-cancel-abort: ssid=" + selectedSSID + " radio did not confirm stop, not retrying");
      if (autoConnecting) {
        handleAutoConnectFailure();
        return;
      }
      state = WifiSelectionState::CONNECTION_FAILED;
      requestUpdate();
      return;
    }

    if (progress == CancelProgress::ConfirmedAfterRadioStop) {
      // The escalation stopped and deinitialised the radio, so it has to be brought
      // back up before anything can associate. Untouched on the ordinary
      // event-confirmed path, which needs none of this.
      prepareRadio();
    }
    beginConnection(/*useHint=*/false);
    return;
  }

  if (status == WL_CONNECTED && !completionBelongsToActiveAttempt()) {
    return;  // Withheld; the ordinary connection timeout still bounds this.
  }

  if (status == WL_CONNECTED) {
    // Successfully connected
    IPAddress ip = WiFi.localIP();
    char ipStr[16];
    snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    connectedIP = ipStr;
    autoConnecting = false;
    HEAP_TRACE_MARK("WIFI.connected", "EVENT");
    // elapsedMs is association only -- WiFi.begin() to WL_CONNECTED/GOT_IP. Total
    // "WiFi ready" is reported separately once the clock sync has also settled, since
    // that is no longer part of this measurement.
    associationMs = millis() - connectionStartTime;
    WifiDiagLog::append("connected: ssid=" + selectedSSID + " ip=" + std::string(ipStr) +
                        " elapsedMs=" + std::to_string(associationMs));
#ifndef SIMULATOR
    {
      uint8_t traceBssid[wifi_connect_policy::BSSID_LEN] = {};
      WiFi.BSSID(traceBssid);
      HEAP_TRACE_EVENT("TEMP:WIFI.gotip", "attempt=%lu mode=%s bssid=%02x%02x%02x%02x%02x%02x ch=%d assocMs=%lu",
                       static_cast<unsigned long>(attemptId), attemptChannel != 0 ? "hinted" : "fullscan",
                       traceBssid[0], traceBssid[1], traceBssid[2], traceBssid[3], traceBssid[4], traceBssid[5],
                       WiFi.channel(), associationMs);
    }
#endif

// Guarded out of the simulator build: crosspoint-simulator's WiFiClass stub
// doesn't implement BSSID()/channel() either.
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2 && !defined(SIMULATOR)
    uint8_t connectedBssid[6] = {};
    WiFi.BSSID(connectedBssid);
    LOG_DBG("WIFI", "Connected BSSID: %02x:%02x:%02x:%02x:%02x:%02x, channel: %d, RSSI: %d dBm",
            static_cast<unsigned>(connectedBssid[0]), static_cast<unsigned>(connectedBssid[1]),
            static_cast<unsigned>(connectedBssid[2]), static_cast<unsigned>(connectedBssid[3]),
            static_cast<unsigned>(connectedBssid[4]), static_cast<unsigned>(connectedBssid[5]), WiFi.channel(),
            WiFi.RSSI());
#endif

    // System time must be roughly correct for TLS certificate validation -- OPDS
    // fetches, OTA update checks, and font downloads are all HTTPS. Neither the
    // DS3231 (X3 -- hour/minute only, no calendar date) nor X4 (no RTC at all) can
    // preserve the date across a reboot, so without re-syncing here every HTTPS
    // request silently fails cert validation after every restart (confirmed via a
    // real device: "Foulad eBooks" repeatedly failed with a bare ESP_ERR_HTTP_CONNECT
    // right after boot, with healthy heap and a healthy server -- the system clock
    // was the missing piece). Cheap to check on every connection: skipped entirely
    // once time is already valid, so this doesn't add the ~5s SNTP wait on every
    // WiFi connect, only right after a cold boot/OTA restart.
    // Started, not waited on: nothing on this screen needs the clock, so the up-to-5s
    // SNTP wait belongs in loop() (see finishWhenClockSynced) rather than in front of
    // onComplete(). Guarded out of the simulator build: crosspoint-simulator's own
    // HalClock stub doesn't implement these (its network stack isn't real
    // ESP-IDF/mbedTLS, so the cert-validation clock issue this works around doesn't
    // apply there).
#ifndef SIMULATOR
    if (!HalClock::isSystemTimeValid() && HalClock::beginSystemTimeSync()) {
      awaitingTimeSync = true;
      timeSyncStartMs = millis();
      HEAP_TRACE_EVENT("TEMP:WIFI.ntpBegin", "attempt=%lu boundMs=%lu", static_cast<unsigned long>(attemptId),
                       wifi_connect_policy::TIME_SYNC_TIMEOUT_MS);
    }
#endif

    // Save this as the last connected network, plus where it was actually joined so the
    // next attempt can skip the scan - SD card operations need lock as we use SPI for both
    {
      RenderLock lock(*this);
      WIFI_STORE.setLastConnectedSsid(selectedSSID);
      rememberConnectionHint();
    }

    // If we entered a new password, ask if user wants to save it
    // Otherwise fall through to CONNECTED, which completes as soon as the clock is ready
    if (!usedSavedPassword && !enteredPassword.empty()) {
      state = WifiSelectionState::SAVE_PROMPT;
      savePromptSelection = 0;  // Default to "Yes"
      requestUpdate();
    } else {
      LOG_DBG("WIFI", "Connected with saved/open credentials");
      state = WifiSelectionState::CONNECTED;
      // Only worth a repaint when there is actually a wait to show; otherwise loop()
      // completes on the very next tick and the paint would never be seen.
      if (awaitingTimeSync) {
        requestUpdate();
      }
    }
    return;
  }

  // A pinned attempt that hard-fails or stalls means the AP moved (roamed, changed
  // channel) or is gone. Retry once with a full scan before reporting failure or moving
  // on to the next saved network, and drop the stale hint so the next connect starts
  // clean instead of paying for it again.
  if (attemptChannel != 0 && !hintFallbackDone) {
    const bool hintHardFail = (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL);
    if (hintHardFail || millis() - connectionStartTime > wifi_connect_policy::HINT_ATTEMPT_TIMEOUT_MS) {
      WifiDiagLog::append("hint-fallback: ssid=" + selectedSSID + " reason=" + (hintHardFail ? "failed" : "timeout"));
      requestHintCancellation(hintHardFail);
      return;
    }
  }

  if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
    connectionError = tr(STR_ERROR_GENERAL_FAILURE);
    if (status == WL_NO_SSID_AVAIL) {
      connectionError = tr(STR_ERROR_NETWORK_NOT_FOUND);
    }
    WifiDiagLog::append("failed: ssid=" + selectedSSID +
                        " status=" + (status == WL_NO_SSID_AVAIL ? "NO_SSID_AVAIL" : "CONNECT_FAILED") +
                        " auto=" + (autoConnecting ? "1" : "0"));
    if (autoConnecting) {
      handleAutoConnectFailure();
      return;
    }
    state = WifiSelectionState::CONNECTION_FAILED;
    requestUpdate();
    return;
  }

  // Check for timeout. Auto-connect uses the shorter budget so a saved network that's
  // merely out of range doesn't eat 15s before moving on to the next one; see
  // wifi_connect_policy for the measurements both budgets are sized against.
  const unsigned long timeoutMs =
      autoConnecting ? wifi_connect_policy::AUTO_CONNECTION_TIMEOUT_MS : wifi_connect_policy::CONNECTION_TIMEOUT_MS;
  if (millis() - connectionStartTime > timeoutMs) {
    WiFi.disconnect();
    connectionError = tr(STR_ERROR_CONNECTION_TIMEOUT);
    WifiDiagLog::append("timeout: ssid=" + selectedSSID + " afterMs=" + std::to_string(timeoutMs) + " auto=" +
                        (autoConnecting ? "1" : "0") + " lastStatus=" + std::to_string(static_cast<int>(status)));
    if (autoConnecting) {
      handleAutoConnectFailure();
      return;
    }
    state = WifiSelectionState::CONNECTION_FAILED;
    requestUpdate();
    return;
  }
}

void WifiSelectionActivity::finishWhenClockSynced() {
  if (awaitingTimeSync) {
    const bool synced = HalClock::isSystemTimeSyncComplete();
    if (!synced && millis() - timeSyncStartMs < wifi_connect_policy::TIME_SYNC_TIMEOUT_MS) {
      return;  // Poll again next tick; the connected screen is already up.
    }
    awaitingTimeSync = false;
    HEAP_TRACE_EVENT("TEMP:WIFI.ntpEnd", "attempt=%lu synced=%d afterMs=%lu boundMs=%lu",
                     static_cast<unsigned long>(attemptId), synced ? 1 : 0, millis() - timeSyncStartMs,
                     wifi_connect_policy::TIME_SYNC_TIMEOUT_MS);
    if (!synced) {
      LOG_ERR("WIFI", "NTP sync did not land; HTTPS cert validation may fail until it does");
    }
  }

  // Sync RTC from NTP on the first successful WiFi connection only. The DS3231
  // drifts ~2 ppm so one sync is enough; users can force a re-sync from
  // Settings > Customise Status Bar > Sync clock now.
  if (halClock.isAvailable() && !SETTINGS.clockHasBeenSynced) {
    if (halClock.syncFromNTP()) {
      SETTINGS.clockHasBeenSynced = 1;
      SETTINGS.saveToFile();
    }
  }

  // Association and total WiFi-ready are separate figures now that the clock sync runs
  // asynchronously: assocMs ends at GOT_IP, readyMs at the point the parent may use the
  // connection for TLS.
  HEAP_TRACE_EVENT("TEMP:WIFI.ready", "attempt=%lu assocMs=%lu readyMs=%lu", static_cast<unsigned long>(attemptId),
                   associationMs, millis() - connectionStartTime);
  onComplete(true);
}

void WifiSelectionActivity::loop() {
  // Check scan progress
  if (state == WifiSelectionState::SCANNING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      WiFi.scanDelete();
      onComplete(false);
      return;
    }
    if (autoConnecting && mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      autoConnecting = false;
      manualNetworkListRequested = true;
      requestUpdate();
    }
    processWifiScanResults();
    return;
  }

  // Check connection progress
  if (state == WifiSelectionState::CONNECTING || state == WifiSelectionState::AUTO_CONNECTING) {
    if (state == WifiSelectionState::AUTO_CONNECTING) {
      // Auto-connect can be interrupted: Back cancels outright, Confirm drops
      // straight to the manual scan list instead of waiting out the sequence.
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        WiFi.disconnect();
        onComplete(false);
        return;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        showNetworkListFromAutoConnect();
        return;
      }
    }
    checkConnectionStatus();
    return;
  }

  // Reached once the hidden-network SSID has been entered (and was non-empty).
  if (state == WifiSelectionState::HIDDEN_SSID_ENTRY) {
    const auto savedCred = WIFI_STORE.findCredential(selectedSSID);
    if (savedCred && !savedCred->password.empty()) {
      // We already know this hidden network - connect with the saved password
      enteredPassword = savedCred->password;
      usedSavedPassword = true;
      LOG_DBG("WiFi", "Using saved password for hidden network %s", selectedSSID.c_str());
      attemptConnection();
    } else {
      // Prompt for the password (empty password connects to open hidden APs)
      promptPasswordEntry();
    }
    return;
  }

  if (state == WifiSelectionState::PASSWORD_ENTRY) {
    // Reach here once password entry finished in subactivity
    attemptConnection();
    return;
  }

  // Handle save prompt state
  if (state == WifiSelectionState::SAVE_PROMPT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (savePromptSelection > 0) {
        savePromptSelection--;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
               mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (savePromptSelection < 1) {
        savePromptSelection++;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (savePromptSelection == 0) {
        // User chose "Yes" - save the password
        RenderLock lock(*this);
        WIFI_STORE.addCredential(selectedSSID, enteredPassword);
      }
      // Parent will start the web server, once the clock is usable for TLS
      state = WifiSelectionState::CONNECTED;
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      // Skip saving, complete anyway
      state = WifiSelectionState::CONNECTED;
    }
    return;
  }

  // Handle forget prompt state (connection failed with saved credentials)
  if (state == WifiSelectionState::FORGET_PROMPT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (forgetPromptSelection > 0) {
        forgetPromptSelection--;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
               mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (forgetPromptSelection < 1) {
        forgetPromptSelection++;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (forgetPromptSelection == 1) {
        RenderLock lock(*this);
        // User chose "Forget network" - forget the network
        WIFI_STORE.removeCredential(selectedSSID);
        // Update the network list to reflect the change
        const auto network = find_if(networks.begin(), networks.end(),
                                     [this](const WifiNetworkInfo& net) { return net.ssid == selectedSSID; });
        if (network != networks.end()) {
          network->hasSavedPassword = false;
        }
      }
      // Go back to network list (whether Cancel or Forget network was selected)
      startWifiScan();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      // Skip forgetting, go back to network list
      startWifiScan();
    }
    return;
  }

  // Connected: hand the result to the parent as soon as the clock is usable for TLS.
  if (state == WifiSelectionState::CONNECTED) {
    finishWhenClockSynced();
    return;
  }

  // Handle connection failed state
  if (state == WifiSelectionState::CONNECTION_FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      // If we were auto-connecting or using a saved credential, offer to forget
      // the network
      if (autoConnecting || usedSavedPassword) {
        autoConnecting = false;
        state = WifiSelectionState::FORGET_PROMPT;
        forgetPromptSelection = 0;  // Default to "Cancel"
      } else {
        // Go back to network list on failure for non-saved credentials
        state = WifiSelectionState::NETWORK_LIST;
      }
      requestUpdate();
      return;
    }
  }

  // Handle network list state
  if (state == WifiSelectionState::NETWORK_LIST) {
    if (manualNetworkListRequested) {
      if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
        manualNetworkListRequested = false;
      }
      return;
    }

    // Check for Back button to exit (cancel)
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      onComplete(false);
      return;
    }

    // Check for Confirm button to select network or rescan
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!networks.empty()) {
        selectNetwork(selectedNetworkIndex);
      } else {
        startWifiScan();
      }
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      startWifiScan();
      return;
    }

    const bool leftPressed = mappedInput.wasPressed(MappedInputManager::Button::Left);
    if (leftPressed) {
      const bool hasSavedPassword = !networks.empty() && networks[selectedNetworkIndex].hasSavedPassword;
      if (hasSavedPassword) {
        selectedSSID = networks[selectedNetworkIndex].ssid;
        state = WifiSelectionState::FORGET_PROMPT;
        forgetPromptSelection = 0;  // Default to "Cancel"
        requestUpdate();
        return;
      }
    }

    // Handle navigation
    buttonNavigator.onNext([this] {
      selectedNetworkIndex = ButtonNavigator::nextIndex(selectedNetworkIndex, networks.size());
      requestUpdate();
    });

    buttonNavigator.onPrevious([this] {
      selectedNetworkIndex = ButtonNavigator::previousIndex(selectedNetworkIndex, networks.size());
      requestUpdate();
    });
  }
}

std::string WifiSelectionActivity::getSignalStrengthIndicator(const int32_t rssi) const {
  // Convert RSSI to signal bars representation
  if (rssi >= -50) {
    return "||||";  // Excellent
  }
  if (rssi >= -60) {
    return " |||";  // Good
  }
  if (rssi >= -70) {
    return "  ||";  // Fair
  }
  return "   |";  // Very weak
}

void WifiSelectionActivity::render(RenderLock&&) {
  // Don't render if we're in a keyboard-entry state - we're just transitioning
  // from the keyboard subactivity back to the main activity
  if (state == WifiSelectionState::PASSWORD_ENTRY || state == WifiSelectionState::HIDDEN_SSID_ENTRY) {
    return;
  }

  renderer.clearScreen();

  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  // Draw header
  // 64, not 32: STR_NETWORKS_FOUND is UTF-8 (e.g. Arabic "تم العثور على %zu شبكة" is 37
  // bytes alone vs 22 codepoints) -- a tight buffer truncates mid-multibyte-sequence.
  // realNetworkCount, not networks.size(): excludes the synthetic "Add hidden
  // network..." entry, which isn't a network the scan actually found.
  char countStr[64];
  snprintf(countStr, sizeof(countStr), tr(STR_NETWORKS_FOUND), realNetworkCount);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_WIFI_NETWORKS), countStr);
  GUI.drawSubHeader(
      renderer,
      Rect{screen.x, screen.y + metrics.topPadding + metrics.headerHeight, screen.width, metrics.tabBarHeight},
      cachedMacAddress.c_str());

  switch (state) {
    case WifiSelectionState::AUTO_CONNECTING:
      renderConnecting(&screen, &metrics);
      break;
    case WifiSelectionState::SCANNING:
      renderConnecting(&screen, &metrics);  // Reuse connecting screen with different message
      break;
    case WifiSelectionState::NETWORK_LIST:
      renderNetworkList(&screen, &metrics);
      break;
    case WifiSelectionState::CONNECTING:
      renderConnecting(&screen, &metrics);
      break;
    case WifiSelectionState::CONNECTED:
      renderConnected(&screen, &metrics);
      break;
    case WifiSelectionState::SAVE_PROMPT:
      renderSavePrompt(&screen, &metrics);
      break;
    case WifiSelectionState::CONNECTION_FAILED:
      renderConnectionFailed(&screen, &metrics);
      break;
    case WifiSelectionState::FORGET_PROMPT:
      renderForgetPrompt(&screen, &metrics);
      break;
  }

  renderer.displayBuffer();
}

void WifiSelectionActivity::renderNetworkList(const Rect* screen, const ThemeMetrics* metrics) const {
  if (networks.empty()) {
    // No networks found or scan failed
    const auto height = renderer.getLineHeight(UI_10_FONT_ID);
    const auto top = screen->y + (screen->height - height) / 2;
    UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top, tr(STR_NO_NETWORKS));
    UITheme::drawCenteredText(renderer, *screen, SMALL_FONT_ID, top + height + 10, tr(STR_PRESS_OK_SCAN));
  } else {
    int contentTop =
        screen->y + metrics->topPadding + metrics->headerHeight + metrics->tabBarHeight + metrics->verticalSpacing;
    int contentHeight = screen->height - contentTop - metrics->verticalSpacing * 2;
    GUI.drawList(
        renderer, Rect{screen->x, contentTop, screen->width, contentHeight}, static_cast<int>(networks.size()),
        selectedNetworkIndex,
        [this](int index) {
          const auto& network = networks[index];
          return network.isHiddenPlaceholder ? std::string(tr(STR_ADD_HIDDEN_NETWORK)) : network.ssid;
        },
        nullptr, nullptr,
        [this](int index) {
          const auto& network = networks[index];
          if (network.isHiddenPlaceholder) {
            return std::string();
          }
          return std::string(network.hasSavedPassword ? "+ " : "") + (network.isEncrypted ? "* " : "") +
                 getSignalStrengthIndicator(network.rssi);
        });
  }

  GUI.drawHelpText(renderer,
                   Rect{screen->x, screen->y + screen->height - metrics->contentSidePadding - 15, screen->width, 20},
                   tr(STR_NETWORK_LEGEND));

  const bool hasSavedPassword = !networks.empty() && networks[selectedNetworkIndex].hasSavedPassword;
  const char* forgetLabel = hasSavedPassword ? tr(STR_FORGET_BUTTON) : "";

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONNECT), forgetLabel, tr(STR_RETRY));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderConnecting(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height) / 2;

  if (state == WifiSelectionState::SCANNING) {
    UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top,
                              autoConnecting ? tr(STR_FINDING_SAVED_WIFI) : tr(STR_SCANNING));
    if (autoConnecting) {
      const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_SHOW_NETWORKS), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else {
    UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top - 40,
                              autoConnecting ? tr(STR_CONNECTING_SAVED_WIFI) : tr(STR_CONNECTING), true,
                              EpdFontFamily::BOLD);

    std::string ssidInfo = std::string(tr(STR_TO_PREFIX)) + selectedSSID;
    if (ssidInfo.length() > 25) {
      ssidInfo.replace(22, ssidInfo.length() - 22, "...");
    }
    UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top, ssidInfo.c_str());
    if (autoConnecting) {
      const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_SHOW_NETWORKS), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  }
}

void WifiSelectionActivity::renderConnected(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height * 4) / 2;

  UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top - 30, tr(STR_CONNECTED), true, EpdFontFamily::BOLD);

  std::string ssidInfo = std::string(tr(STR_NETWORK_PREFIX)) + selectedSSID;
  if (ssidInfo.length() > 28) {
    ssidInfo.replace(25, ssidInfo.length() - 25, "...");
  }
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + 10, ssidInfo.c_str());

  const std::string ipInfo = std::string(tr(STR_IP_ADDRESS_PREFIX)) + connectedIP;
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + 40, ipInfo.c_str());

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels("", tr(STR_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderSavePrompt(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height * 3) / 2;

  UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top - 40, tr(STR_CONNECTED), true, EpdFontFamily::BOLD);

  std::string ssidInfo = std::string(tr(STR_NETWORK_PREFIX)) + selectedSSID;
  if (ssidInfo.length() > 28) {
    ssidInfo.replace(25, ssidInfo.length() - 25, "...");
  }
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top, ssidInfo.c_str());

  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + 40, tr(STR_SAVE_PASSWORD));

  // Draw Yes/No buttons
  const int buttonY = top + 80;
  constexpr int buttonWidth = 60;
  constexpr int buttonSpacing = 30;
  constexpr int totalWidth = buttonWidth * 2 + buttonSpacing;
  const int startX = screen->x + (screen->width - totalWidth) / 2;

  // Draw "Yes" button
  if (savePromptSelection == 0) {
    std::string text = "[" + std::string(tr(STR_YES)) + "]";
    renderer.drawText(UI_10_FONT_ID, startX, buttonY, text.c_str());
  } else {
    renderer.drawText(UI_10_FONT_ID, startX + 4, buttonY, tr(STR_YES));
  }

  // Draw "No" button
  if (savePromptSelection == 1) {
    std::string text = "[" + std::string(tr(STR_NO)) + "]";
    renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing, buttonY, text.c_str());
  } else {
    renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing + 4, buttonY, tr(STR_NO));
  }

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderConnectionFailed(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height * 2) / 2;

  UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top - 20, tr(STR_CONNECTION_FAILED), true,
                            EpdFontFamily::BOLD);
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + 20, connectionError.c_str());

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderForgetPrompt(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height * 3) / 2;

  UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top - 40, tr(STR_FORGET_NETWORK), true,
                            EpdFontFamily::BOLD);

  std::string ssidInfo = std::string(tr(STR_NETWORK_PREFIX)) + selectedSSID;
  if (ssidInfo.length() > 28) {
    ssidInfo.replace(25, ssidInfo.length() - 25, "...");
  }
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top, ssidInfo.c_str());

  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + 40, tr(STR_FORGET_AND_REMOVE));

  // Draw Cancel/Forget network buttons
  const int buttonY = top + 80;
  constexpr int buttonWidth = 120;
  constexpr int buttonSpacing = 30;
  constexpr int totalWidth = buttonWidth * 2 + buttonSpacing;
  const int startX = screen->x + (screen->width - totalWidth) / 2;

  // Draw "Cancel" button
  if (forgetPromptSelection == 0) {
    std::string text = "[" + std::string(tr(STR_CANCEL)) + "]";
    renderer.drawText(UI_10_FONT_ID, startX, buttonY, text.c_str());
  } else {
    renderer.drawText(UI_10_FONT_ID, startX + 4, buttonY, tr(STR_CANCEL));
  }

  // Draw "Forget network" button
  if (forgetPromptSelection == 1) {
    std::string text = "[" + std::string(tr(STR_FORGET_BUTTON)) + "]";
    renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing, buttonY, text.c_str());
  } else {
    renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing + 4, buttonY, tr(STR_FORGET_BUTTON));
  }

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::onComplete(const bool connected) {
  // Counted, not just logged: a single "completed" line proves nothing about
  // exactly-once, whereas a second line carrying n=2 would be the failure itself.
  ++completionCount;
  HEAP_TRACE_EVENT("TEMP:WIFI.complete", "n=%u attempt=%lu connected=%d", static_cast<unsigned>(completionCount),
                   static_cast<unsigned long>(attemptId), connected ? 1 : 0);

  ActivityResult result;
  result.isCancelled = !connected;
  if (connected) {
    result.data = WifiResult{true, selectedSSID, connectedIP};
  }
  setResult(std::move(result));
  finish();
}
