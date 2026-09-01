#include "WifiConnectionHintStore.h"

#include <Logging.h>

#include <algorithm>
#include <cstring>

void WifiConnectionHintStore::toJson(JsonDocument& doc) const {
  std::lock_guard<std::mutex> lock(hintMutex);

  JsonArray arr = doc["hints"].to<JsonArray>();
  for (const auto& entry : entries) {
    char bssidText[wifi_connect_policy::BSSID_TEXT_LEN + 1];
    wifi_connect_policy::formatBssid(entry.hint.bssid, bssidText);

    JsonObject obj = arr.add<JsonObject>();
    obj["ssid"] = entry.ssid;
    obj["bssid"] = bssidText;
    obj["channel"] = entry.hint.channel;
  }
}

bool WifiConnectionHintStore::fromJson(JsonVariantConst doc) {
  std::lock_guard<std::mutex> lock(hintMutex);

  entries.clear();
  JsonArrayConst arr = doc["hints"].as<JsonArrayConst>();
  entries.reserve(std::min(arr.size(), MAX_HINTS));

  for (JsonObjectConst obj : arr) {
    if (entries.size() >= MAX_HINTS) break;

    Entry entry;
    entry.ssid = obj["ssid"] | "";
    if (entry.ssid.empty()) continue;

    // Anything that does not validate is dropped rather than repaired: a hint is a
    // performance cache, and a wrong one would cost a retry on every connect.
    if (!wifi_connect_policy::parseBssid(obj["bssid"] | "", entry.hint.bssid)) continue;
    const int channel = obj["channel"] | 0;
    if (!wifi_connect_policy::isValidChannel(channel)) continue;
    entry.hint.channel = static_cast<uint8_t>(channel);
    if (!wifi_connect_policy::isUsableHint(entry.hint.bssid, entry.hint.channel)) continue;

    entries.push_back(std::move(entry));
  }

  LOG_DBG("WHS", "Loaded %zu WiFi connection hints from file", entries.size());
  return true;
}

std::optional<WifiConnectionHint> WifiConnectionHintStore::find(const std::string& ssid) const {
  std::lock_guard<std::mutex> lock(hintMutex);

  const auto entry = std::find_if(entries.begin(), entries.end(), [&ssid](const Entry& e) { return e.ssid == ssid; });
  if (entry == entries.end()) return std::nullopt;
  if (!wifi_connect_policy::isUsableHint(entry->hint.bssid, entry->hint.channel)) return std::nullopt;
  return entry->hint;
}

bool WifiConnectionHintStore::remember(const std::string& ssid, const uint8_t* bssid, const uint8_t channel) {
  if (ssid.empty() || !wifi_connect_policy::isUsableHint(bssid, channel)) return false;

  {
    std::lock_guard<std::mutex> lock(hintMutex);

    auto entry = std::find_if(entries.begin(), entries.end(), [&ssid](const Entry& e) { return e.ssid == ssid; });
    if (entry != entries.end()) {
      // Unchanged hint: skip the serialize + SD write entirely (every successful
      // connect to a stable AP would otherwise rewrite the same bytes).
      if (entry->hint.channel == channel &&
          std::memcmp(entry->hint.bssid, bssid, wifi_connect_policy::BSSID_LEN) == 0) {
        return true;
      }
    } else {
      // Oldest entry loses: hints are disposable, and the list is only ever as useful
      // as the networks the device still visits.
      if (entries.size() >= MAX_HINTS) entries.erase(entries.begin());
      entries.push_back(Entry{ssid, WifiConnectionHint{}});
      entry = entries.end() - 1;
    }

    std::memcpy(entry->hint.bssid, bssid, wifi_connect_policy::BSSID_LEN);
    entry->hint.channel = channel;
  }
  return saveToFile();
}

bool WifiConnectionHintStore::forget(const std::string& ssid) {
  {
    std::lock_guard<std::mutex> lock(hintMutex);
    const auto entry = std::find_if(entries.begin(), entries.end(), [&ssid](const Entry& e) { return e.ssid == ssid; });
    if (entry == entries.end()) return false;
    entries.erase(entry);
  }
  return saveToFile();
}
