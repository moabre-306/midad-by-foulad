#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "network/WifiConnectPolicy.h"

// Where a given SSID was last actually joined. Lets WiFi.begin() associate on one
// channel against one AP instead of scanning every channel first.
struct WifiConnectionHint {
  uint8_t bssid[wifi_connect_policy::BSSID_LEN] = {};
  uint8_t channel = 0;  // 0 means "nothing remembered"
};

/**
 * Singleton store for per-SSID BSSID/channel connection hints, kept separate from
 * WifiCredentialStore so a purely-derived performance cache can never corrupt or
 * complicate the credential file that owns the passwords.
 *
 * Hints are disposable by construction: every read is validated, anything that does not
 * survive validation is ignored, and a wrong hint costs one short retry
 * (wifi_connect_policy::HINT_ATTEMPT_TIMEOUT_MS) before the full-scan path takes over.
 */
class WifiConnectionHintStore : public PersistableStore<WifiConnectionHintStore> {
 private:
  struct Entry {
    std::string ssid;
    WifiConnectionHint hint;
  };

  std::vector<Entry> entries;
  // Guards the in-memory entries independently of PersistableStore's file mutex, as
  // WifiCredentialStore does for the same reason: readers must never wait on SD I/O.
  mutable std::mutex hintMutex;

  // Matches WifiCredentialStore::MAX_NETWORKS -- a hint is only ever useful for a
  // network we also hold credentials for.
  static constexpr size_t MAX_HINTS = 8;

  WifiConnectionHintStore() = default;

  friend class PersistableStore<WifiConnectionHintStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/wifi_hints.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Returns the hint for ssid only when it passes wifi_connect_policy::isUsableHint.
  std::optional<WifiConnectionHint> find(const std::string& ssid) const;

  // Records where ssid was just joined. Writes to SD only when the stored value
  // actually changes, so a stable network costs nothing on repeat connects.
  bool remember(const std::string& ssid, const uint8_t* bssid, uint8_t channel);

  // Drops a hint that failed to associate. No-op (and no write) if none was stored.
  bool forget(const std::string& ssid);
};

#define WIFI_HINTS WifiConnectionHintStore::getInstance()
