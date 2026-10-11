#include "TrmnlClient.h"

#include <ArduinoJson.h>
#include <BatteryMonitor.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>
#include <WiFi.h>
#include <esp_mac.h>

#include <algorithm>
#include <utility>
#include <vector>

#include "TrmnlStore.h"
#include "WifiCredentialStore.h"
#include "network/HttpDownloader.h"

namespace TrmnlClient {
namespace {

constexpr char TAG[] = "TRMNL";
constexpr char SCREEN_DIR[] = "/.crosspoint/trmnl";
constexpr char DOWNLOAD_PATH[] = "/.crosspoint/trmnl/download.tmp";

// trmnl.app picks image size, bit depth and features from the reported model
// and firmware version. The X4 Pro shares the X4's 800x480 panel.
constexpr char TRMNL_FW_VERSION[] = "1.8.16";

constexpr uint32_t MIN_REFRESH_SECONDS = 60;
constexpr uint32_t MAX_REFRESH_SECONDS = 24 * 60 * 60;
// Retry delays after consecutive failures: 1 min, 5 min, then the refresh rate.
constexpr uint32_t RETRY_SECONDS[] = {60, 300};

const char* deviceModel() { return gpio.deviceIsX3() ? "xteink_x3" : "xteink_x4"; }

std::string batteryVoltage() {
  const BatteryMonitor battery;
  const auto status = battery.readStatus();
  float volts = 0.0f;
  if (status.millivoltsKnown && status.millivolts > 0) {
    volts = status.millivolts / 1000.0f;
  } else {
    // Gauge boards that only report a percentage: map it onto a 1S Li-ion range.
    volts = 3.0f + 1.2f * (static_cast<float>(powerManager.getBatteryPercentage()) / 100.0f);
  }
  char buf[8];
  snprintf(buf, sizeof(buf), "%.2f", volts);
  return buf;
}

std::vector<std::pair<std::string, std::string>> baseHeaders() {
  std::vector<std::pair<std::string, std::string>> headers;
  headers.reserve(10);
  headers.emplace_back("ID", macAddress());
  headers.emplace_back("FW-Version", TRMNL_FW_VERSION);
  headers.emplace_back("Model", deviceModel());
  headers.emplace_back("Width", std::to_string(display.getDisplayWidth()));
  headers.emplace_back("Height", std::to_string(display.getDisplayHeight()));
  return headers;
}

// scheme://host[:port], used to keep the access token off third-party image hosts.
std::string urlOrigin(const std::string& url) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return url;
  const size_t hostEnd = url.find_first_of("/?#", schemeEnd + 3);
  return url.substr(0, hostEnd);
}

uint32_t clampRefresh(const uint32_t seconds) { return std::clamp(seconds, MIN_REFRESH_SECONDS, MAX_REFRESH_SECONDS); }

uint32_t retryDelay(const uint8_t failCount) {
  if (failCount >= 1 && failCount <= std::size(RETRY_SECONDS)) return RETRY_SECONDS[failCount - 1];
  return clampRefresh(TRMNL_STORE.refreshSeconds);
}

bool fetchJson(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
               JsonDocument& doc) {
  std::string body;
  {
    HttpDownloader::ScopedHeaders scoped(headers);
    if (!HttpDownloader::fetchUrl(url, body)) {
      const auto failure = HttpDownloader::getLastFailure();
      LOG_ERR(TAG, "GET %s failed (stage=%d detail=%d)", url.c_str(), static_cast<int>(failure.stage), failure.detail);
      return false;
    }
  }
  const auto err = deserializeJson(doc, body);
  if (err) {
    LOG_ERR(TAG, "Bad JSON from %s: %s", url.c_str(), err.c_str());
    return false;
  }
  return true;
}

enum class ImageType { Unknown, Bmp, Png, Jpeg };

ImageType sniffImage(const char* path) {
  HalFile file;
  if (!Storage.openFileForRead(TAG, path, file)) return ImageType::Unknown;
  uint8_t magic[4] = {};
  if (file.read(magic, sizeof(magic)) != sizeof(magic)) return ImageType::Unknown;
  if (magic[0] == 'B' && magic[1] == 'M') return ImageType::Bmp;
  if (magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G') return ImageType::Png;
  if (magic[0] == 0xFF && magic[1] == 0xD8) return ImageType::Jpeg;
  return ImageType::Unknown;
}

// Turns the downloaded file into SCREEN_PATH. BMPs are used as-is; PNG/JPEG are
// decoded at native size to a 2-bit grayscale BMP.
bool storeScreen() {
  const ImageType type = sniffImage(DOWNLOAD_PATH);
  if (type == ImageType::Unknown) {
    LOG_ERR(TAG, "Unsupported image format");
    Storage.remove(DOWNLOAD_PATH);
    return false;
  }
  if (Storage.exists(SCREEN_PATH)) Storage.remove(SCREEN_PATH);

  if (type == ImageType::Bmp) {
    return Storage.rename(DOWNLOAD_PATH, SCREEN_PATH);
  }

  bool ok = false;
  {
    HalFile src;
    HalFile dst;
    if (Storage.openFileForRead(TAG, DOWNLOAD_PATH, src) && Storage.openFileForWrite(TAG, SCREEN_PATH, dst)) {
      ok = type == ImageType::Png ? PngToBmpConverter::pngFileToBmpStreamWithSize(src, dst, 0, 0)
                                  : JpegToBmpConverter::jpegFileToBmpStreamWithSize(src, dst, 0, 0);
    }
  }
  Storage.remove(DOWNLOAD_PATH);
  if (!ok) {
    LOG_ERR(TAG, "Image conversion failed");
    Storage.remove(SCREEN_PATH);
  }
  return ok;
}

Result finish(const Result result, uint32_t& nextRefreshSeconds) {
  if (result == Result::Ok || result == Result::Unchanged) {
    TRMNL_STORE.failCount = 0;
    nextRefreshSeconds = clampRefresh(TRMNL_STORE.refreshSeconds);
  } else {
    if (TRMNL_STORE.failCount < UINT8_MAX) TRMNL_STORE.failCount++;
    nextRefreshSeconds = retryDelay(TRMNL_STORE.failCount);
  }
  TRMNL_STORE.saveToFile();
  return result;
}

}  // namespace

bool connectSavedWifi(const uint32_t timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (Storage.exists(WifiCredentialStore::getFilePath())) WIFI_STORE.loadFromFile();

  std::optional<WifiCredential> cred;
  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (!lastSsid.empty()) cred = WIFI_STORE.findCredential(lastSsid);
  if (!cred) cred = WIFI_STORE.getCredentialAt(0);
  if (!cred) {
    LOG_ERR(TAG, "No saved Wi-Fi network");
    return false;
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  if (cred->password.empty()) {
    WiFi.begin(cred->ssid.c_str());
  } else {
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  }
  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > timeoutMs) {
      LOG_ERR(TAG, "Wi-Fi connect to %s timed out", cred->ssid.c_str());
      return false;
    }
    delay(100);
  }
  LOG_INF(TAG, "Wi-Fi connected to %s (rssi=%d)", cred->ssid.c_str(), static_cast<int>(WiFi.RSSI()));
  return true;
}

void disconnectWifi() {
  if (WiFi.getMode() == WIFI_MODE_NULL) return;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

std::string macAddress() {
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return buf;
}

Result linkDevice() {
  JsonDocument doc;
  if (!fetchJson(TRMNL_STORE.getBaseUrl() + "/api/setup", baseHeaders(), doc)) return Result::ServerError;

  const int status = doc["status"] | 0;
  const char* apiKey = doc["api_key"] | "";
  if (status != 200 || apiKey[0] == '\0') {
    LOG_ERR(TAG, "Setup refused: status=%d message=%s", status, static_cast<const char*>(doc["message"] | ""));
    return Result::NotRegistered;
  }
  TRMNL_STORE.apiKey = apiKey;
  TRMNL_STORE.friendlyId = doc["friendly_id"] | "";
  TRMNL_STORE.lastFilename.clear();
  TRMNL_STORE.failCount = 0;
  TRMNL_STORE.saveToFile();
  LOG_INF(TAG, "Linked as %s", TRMNL_STORE.friendlyId.c_str());
  return Result::Ok;
}

Result refreshScreen(uint32_t& nextRefreshSeconds) {
  if (!TRMNL_STORE.isLinked()) return finish(Result::NotRegistered, nextRefreshSeconds);

  auto headers = baseHeaders();
  headers.emplace_back("Access-Token", TRMNL_STORE.apiKey);
  headers.emplace_back("Refresh-Rate", std::to_string(clampRefresh(TRMNL_STORE.refreshSeconds)));
  headers.emplace_back("Battery-Voltage", batteryVoltage());
  headers.emplace_back("RSSI", std::to_string(WiFi.RSSI()));

  const std::string baseUrl = TRMNL_STORE.getBaseUrl();
  JsonDocument doc;
  if (!fetchJson(baseUrl + "/api/display", headers, doc)) return finish(Result::ServerError, nextRefreshSeconds);

  const int status = doc["status"] | 0;
  const uint32_t refreshRate = doc["refresh_rate"] | 0u;
  if (refreshRate > 0) TRMNL_STORE.refreshSeconds = clampRefresh(refreshRate);

  const std::string imageUrl = doc["image_url"] | "";
  const std::string filename = doc["filename"] | "";
  if (status == 500 || imageUrl.empty()) {
    LOG_ERR(TAG, "No screen: status=%d", status);
    return finish(status == 500 ? Result::NotRegistered : Result::ServerError, nextRefreshSeconds);
  }

  if (!filename.empty() && filename == TRMNL_STORE.lastFilename && hasScreen()) {
    LOG_DBG(TAG, "Screen unchanged: %s", filename.c_str());
    return finish(Result::Unchanged, nextRefreshSeconds);
  }

  Storage.mkdir(SCREEN_DIR);
  // Only the API host gets the device ID and token; images may live on a CDN.
  const bool sameOrigin = urlOrigin(imageUrl) == urlOrigin(baseUrl);
  const std::vector<std::pair<std::string, std::string>> noHeaders;
  HttpDownloader::DownloadError downloadResult;
  {
    HttpDownloader::ScopedHeaders scoped(sameOrigin ? headers : noHeaders);
    downloadResult = HttpDownloader::downloadToFile(imageUrl, DOWNLOAD_PATH);
  }
  if (downloadResult != HttpDownloader::OK || !storeScreen()) {
    LOG_ERR(TAG, "Image download failed: %s", imageUrl.c_str());
    return finish(Result::ImageError, nextRefreshSeconds);
  }

  TRMNL_STORE.lastFilename = filename;
  LOG_INF(TAG, "New screen %s, next refresh in %lu s", filename.c_str(),
          static_cast<unsigned long>(TRMNL_STORE.refreshSeconds));
  return finish(Result::Ok, nextRefreshSeconds);
}

Result runRefreshCycle(uint32_t& nextRefreshSeconds) {
  constexpr uint32_t WIFI_TIMEOUT_MS = 15000;
  if (!connectSavedWifi(WIFI_TIMEOUT_MS)) {
    disconnectWifi();
    return finish(Result::NoWifi, nextRefreshSeconds);
  }
  const Result result = refreshScreen(nextRefreshSeconds);
  disconnectWifi();
  return result;
}

bool hasScreen() { return Storage.exists(SCREEN_PATH); }

StrId resultMessage(const Result result) {
  switch (result) {
    case Result::Ok:
    case Result::Unchanged:
      return StrId::STR_TRMNL_LINKED;
    case Result::NoWifi:
      return StrId::STR_TRMNL_ERR_NO_WIFI;
    case Result::NotRegistered:
      return StrId::STR_TRMNL_NOT_REGISTERED;
    case Result::ImageError:
      return StrId::STR_TRMNL_ERR_IMAGE;
    case Result::ServerError:
    default:
      return StrId::STR_TRMNL_ERR_SERVER;
  }
}

}  // namespace TrmnlClient
