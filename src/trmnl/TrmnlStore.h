#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

/**
 * TRMNL client configuration and refresh state, on the SD card.
 * The access token is obfuscated on disk like the other stored credentials.
 */
class TrmnlStore : public PersistableStore<TrmnlStore> {
  friend class PersistableStore<TrmnlStore>;
  TrmnlStore() = default;

 public:
  static constexpr char DEFAULT_SERVER_URL[] = "https://trmnl.app";
  static constexpr uint32_t DEFAULT_REFRESH_SECONDS = 900;

  static const char* getFilePath() { return "/.crosspoint/trmnl.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Empty means DEFAULT_SERVER_URL.
  std::string serverUrl;
  std::string apiKey;
  std::string friendlyId;
  // Last refresh_rate the server asked for.
  uint32_t refreshSeconds = DEFAULT_REFRESH_SECONDS;
  // Filename of the image in TrmnlClient::SCREEN_PATH; an unchanged filename skips the repaint.
  std::string lastFilename;
  // Consecutive failed refreshes, for the retry backoff.
  uint8_t failCount = 0;
  bool flipScreen = false;

  // Server URL without a trailing slash, with the default applied.
  std::string getBaseUrl() const;
  bool isLinked() const { return !apiKey.empty(); }
  void unlink();
};

#define TRMNL_STORE TrmnlStore::getInstance()
