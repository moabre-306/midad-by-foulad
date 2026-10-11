#include "TrmnlStore.h"

#include <ObfuscationUtils.h>

void TrmnlStore::toJson(JsonDocument& doc) const {
  doc["serverUrl"] = serverUrl;
  doc["apiKey_obf"] = obfuscation::obfuscateToBase64(apiKey);
  doc["friendlyId"] = friendlyId;
  doc["refreshSeconds"] = refreshSeconds;
  doc["lastFilename"] = lastFilename;
  doc["failCount"] = failCount;
  doc["flipScreen"] = flipScreen;
}

bool TrmnlStore::fromJson(JsonVariantConst doc) {
  serverUrl = doc["serverUrl"] | "";
  apiKey = obfuscation::deobfuscateFromBase64(doc["apiKey_obf"] | "");
  friendlyId = doc["friendlyId"] | "";
  refreshSeconds = doc["refreshSeconds"] | DEFAULT_REFRESH_SECONDS;
  lastFilename = doc["lastFilename"] | "";
  failCount = doc["failCount"] | static_cast<uint8_t>(0);
  flipScreen = doc["flipScreen"] | false;
  return true;
}

std::string TrmnlStore::getBaseUrl() const {
  std::string url = serverUrl.empty() ? std::string(DEFAULT_SERVER_URL) : serverUrl;
  while (!url.empty() && url.back() == '/') url.pop_back();
  return url;
}

void TrmnlStore::unlink() {
  apiKey.clear();
  friendlyId.clear();
  lastFilename.clear();
  failCount = 0;
}
