#pragma once

#include <I18n.h>

#include <cstdint>
#include <string>

/**
 * Minimal TRMNL device-API client (the same calls the TRMNL firmware makes):
 * GET /api/setup links the device, GET /api/display returns the current
 * screen's image URL and refresh rate. Works against trmnl.app (BYOD) and
 * self-hosted servers (BYOS: Terminus, LaraPaper, ...).
 */
namespace TrmnlClient {

enum class Result {
  Ok,
  Unchanged,      // The server still shows the screen that is already cached.
  NoWifi,         // No saved network, or the connection failed.
  NotRegistered,  // The server does not know this MAC address / token.
  ServerError,
  ImageError,
};

// Converted screen image, ready for Bitmap.
constexpr char SCREEN_PATH[] = "/.crosspoint/trmnl/screen.bmp";

// Connects to the last saved Wi-Fi network (or the first one). For the headless
// timer-wake path; the settings screens use WifiSelectionActivity instead.
bool connectSavedWifi(uint32_t timeoutMs);
void disconnectWifi();

// The ID header: the Wi-Fi station MAC, as "AA:BB:CC:DD:EE:FF".
std::string macAddress();

// GET /api/setup; stores the access token and friendly ID on success.
Result linkDevice();

// GET /api/display and download the screen to SCREEN_PATH. nextRefreshSeconds
// is always set: the server's refresh rate, or a retry delay after a failure.
Result refreshScreen(uint32_t& nextRefreshSeconds);

// Headless cycle for a timer wake: connect to saved Wi-Fi, refresh, disconnect.
Result runRefreshCycle(uint32_t& nextRefreshSeconds);

bool hasScreen();

StrId resultMessage(Result result);

}  // namespace TrmnlClient
